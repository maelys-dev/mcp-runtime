# The stdio MCP proxy provider

`maelys_mcp_provider_proxy_spawn` federates a third-party MCP server. It spawns
that server over stdio, speaks real MCP to it as a client, and re-exposes the
tools it finds through `maelys_mcp_provider_create` — the same path an in-process
or `maelys-provider` process provider takes. Effect gating, schema validation,
policy and audit therefore apply to federated traffic unchanged, and nothing
above the provider layer knows the tools came from elsewhere.

```c
maelys_mcp_proxy_options_t options = {
    .executable_path = "/usr/local/bin/some-mcp-server",
    .argv = (char *const[]){"/usr/local/bin/some-mcp-server", "--stdio", NULL},
    .default_effect = MAELYS_MCP_EFFECT_EXECUTE,
    .tool_prefix = "gh."
};
maelys_mcp_provider_t *provider = NULL;
char *error = NULL;
maelys_mcp_provider_proxy_spawn(&options, &provider, &error);
```

## Era negotiation

The two MCP eras have incompatible session models, so the proxy discovers which
one it is talking to rather than being told:

1. It sends `server/discover` carrying the modern per-request `_meta`. A result
   whose `supportedVersions` includes `2026-07-28` settles the era as **modern**,
   and every later request carries the same `_meta`.
2. A JSON-RPC **error**, or a result that is not a discover result, means the
   upstream predates `2026-07-28`. The proxy falls back to **legacy**: an
   `initialize` offering `2025-11-25`, then `notifications/initialized`. Whatever
   supported dated revision the upstream echoes is accepted and recorded, since
   MCP version negotiation lets a server answer with a different one.
3. A **transport** failure is not a fallback signal — there is nothing left to
   hand a handshake to — so it fails the connect with the underlying error.

Everything above happens inside `connect_timeout_ms`, which covers the spawn,
the negotiation and the one `tools/list` together.

## The pinned snapshot

`tools/list` runs exactly once, at connect. That listing is the catalog, and
calls are resolved against it and nothing else; the proxy never re-lists
implicitly. This is the TOCTOU rule from `docs/middleware-design.md`: an
upstream's tool names are remote-controlled and can change between a `list` and
a `call`, so a policy decision taken on a name only means something if the name
is pinned. A call naming a tool absent from the snapshot is refused inside the
provider, before any byte reaches the upstream.

`tool_prefix` applies to the **exposed** name only (`"gh."` exposes upstream
`search` as `gh.search`) and is stripped again when the call is forwarded.

A tool whose `inputSchema` this runtime cannot validate fails the connect,
naming the tool, rather than being quietly dropped from the catalog: a silently
shorter tool list is the kind of difference nobody notices until a call goes
missing. No `outputSchema` is republished — the proxy passes the upstream result
through and will not advertise a guarantee it does not itself enforce.

## Effect mapping

MCP has no effect classes, so `default_effect` is assigned to every upstream
tool. `MAELYS_MCP_EFFECT_UNSPECIFIED` — including an options struct that simply
leaves the field zero — becomes `EXECUTE`, not `READ`. Hosts gate
apply/commit/execute behind `--allow-effect`, so the fallback has to be a gated
class; an ungated one would turn "the caller said nothing" into "anyone may call
it". `EXECUTE` is therefore also the conservative value to pass explicitly.

Inferring an effect from MCP's `readOnlyHint`/`destructiveHint` annotations is a
plausible follow-up. v1 does not do it: those hints are advisory and
upstream-controlled, and a remote-controlled value must not decide a policy
input.

## Progress relay

When a downstream client supplied a progress token, the proxy generates its
**own** token for the upstream leg and puts it in the outbound `_meta` (a bare
`progressToken` key, which is how both eras carry it). Matching
`notifications/progress` frames are queued by the reader thread and drained by
the thread inside the call, which republishes them through
`maelys_mcp_provider_report_progress` under the *client's* token. The proxy's
internal token never reaches the client.

Emission happens on the calling thread, never on the reader thread: the sink
lives on that thread's stack frame and is only that thread's to use, and going
through `sink->emit` is what keeps progress interceptable by a future middleware
sink instead of bypassing it. When the client asked for no progress, no token is
sent and any progress the upstream volunteers anyway is dropped.

## Tolerance policy

The proxy is deliberately more tolerant than the `maelys-provider` wire, because
we do not control upstreams:

| Upstream sends | Proxy does |
|---|---|
| An id-less notification it does not handle (logging, resource updates, anything) | drops it silently |
| `notifications/progress` matching the call in flight | queues it for the calling thread |
| An id-bearing response that is unexpected (wrong id, or nothing outstanding) | protocol failure, faults the transport |
| An id-bearing **request** (sampling, elicitation) | answers `-32601`, keeps the transport |
| Nothing, past the deadline, or EOF | descriptive failure; later calls fail fast against it, keeping the original diagnosis |

Correlated traffic is the part that must stay strict: an unexpected response is
a genuine protocol failure, whereas an unknown notification is noise that must
not fault a transport that is otherwise working.

## Shutdown

Destroying the provider closes the upstream's stdin (the MCP stdio shutdown
signal), waits a bounded time for it to exit, then escalates `SIGTERM` and
`SIGKILL`. The reader thread is unblocked unconditionally before the join, so a
server that ignores EOF cannot wedge the shutdown.

The upstream is spawned with the same scrubbed environment the `maelys-provider`
process provider uses: `PATH`, `LANG`, `LC_ALL` and nothing else. It inherits no
ambient credentials from the runtime.

## v1 limitations

- **No nested-request relay.** A legacy upstream's server-to-client requests are
  refused with `-32601`. Translating them into the resumable `inputRequests`
  shape is named in `docs/middleware-design.md` as the proxy provider's job; it
  is not done here.
- **No MRTR continuation forwarding.** A call carrying `inputResponses` or
  `requestState`, and an upstream result of `input_required`, are both rejected:
  the upstream is a separate conversation and its `requestState` is not this
  runtime's to round-trip.
- **Tools only.** `resources/*` and `prompts/*` are not forwarded.
- **No reconnection.** A dead upstream stays dead for the life of the provider.
- **No live catalog tracking.** `notifications/tools/list_changed` from the
  upstream is dropped, by design: the snapshot is what policy was decided
  against.
- **No environment or working-directory passthrough**, so an upstream that needs
  credentials from the environment cannot get them yet.
