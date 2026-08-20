# Changelog

## Unreleased

- **Tools can now ask the user a question in the middle of a call, and older
  MCP clients understand the question.** A provider handling `tools/call` or
  `resources/read` can open a real `elicitation/create`,
  `sampling/createMessage` or `roots/list` request back at the client on the
  same connection and block for the answer, instead of ending the call and
  waiting to be retried. This is MCP's original multi-round-trip pattern; the
  resumable `input_required` result the runtime already spoke is a
  `2026-07-28` draft type that a schema-validating `2025-11-25` client
  rejects, so a legacy client had no working way to be asked anything.
  Mechanically: each channel keeps a table of outstanding host-to-client
  requests keyed by a host-generated `maelys/nested/<n>` id, and the two
  methods that can block on an answer dispatch on their own thread so the
  connection keeps being read.
- **Concurrent calls on one connection.** `tools/call` and `resources/read`
  no longer occupy the transport reader for their whole duration, so a client
  can have several in flight at once and its cancellations and replies are
  acted on while they run. Every other method still dispatches inline,
  in order, exactly as before; `maelys_mcp_channel_handle` is unchanged and
  still synchronous. The per-channel limit is 8 by default
  (`maelys_mcp_runtime_configure_nested`); beyond it a call waits for the
  channel's admission timeout and is then refused with `-32603` rather than
  queued behind the reader.
- **A nested question can outlive the call deadline it interrupts.** The
  300-second provider call deadline is *suspended* while a nested request is
  outstanding, and the nested request gets its own deadline — ten minutes by
  default — because the thing answering an elicitation may be a person reading
  a diff. Configure both with `maelys_mcp_runtime_configure_nested`.
- **Nothing is left waiting.** A nested request is settled by the client's
  reply, by a `notifications/cancelled` naming the outer call (which now
  reaches through to the call underneath it, whether or not the subscriptions
  module is enabled), by the channel faulting or closing, by the provider
  process dying, or by its deadline — each one covered by a test.
- **Provider protocol `maelys-provider/4` → `/5`**, negotiated as always: the
  host opens at the floor and speaks what the provider declares. Unchanged /3
  and /4 providers keep working — the host now accepts **every** version from
  the floor to the current one rather than only those two endpoints, which is
  a fix as much as a bump: a "floor or newest" check would have started
  rejecting the /4 providers released against 0.13.0. New frames are
  `provider/nestedRequest` and `provider/nestedReply`, correlated by a
  call-scoped `nestedId` and carrying no top-level `id`. See
  `docs/provider-protocol.md`. The bundled SDKs do not yet expose nesting;
  that is the next increment.
- **A provider cannot reach a client surface that was never offered.** A
  nested request naming a method outside the three MCP defines, or one whose
  capability the client did not declare at `initialize`, is refused with
  `denied` before a byte is sent — the same rule `input_required` already
  enforced, so which surfaces a provider may reach does not depend on which
  multi-round-trip shape it chose.
- ABI stays 3. The additions are a new opaque type
  (`maelys_mcp_nested_relay_t`), three new entry points
  (`maelys_mcp_provider_request_client`,
  `maelys_mcp_provider_set_nested_handlers`,
  `maelys_mcp_runtime_configure_nested`) and one new configuration structure;
  no released public layout changed.
- Internal: `maelys_mcp_channel_accept` is the new transport seam — it
  demultiplexes a reply to a nested request from a new request before
  anything is dispatched, and decides what runs on a worker. `stdio.c` is a
  thin adapter over it, and a future HTTP transport reuses it verbatim.
  `maelys_mcp_runtime_dispatch` is unchanged.

## 0.14.0 - 2026-08-20

- **The middleware chain is complete: seven hooks that make the runtime
  programmable.** You can now register middleware that decides, observes and
  transforms everything crossing the runtime — who may call what
  (`on_authorize`), what gets journalled (`on_audit`), what a tool is really
  named and called with (`on_resolve`), whether the provider is invoked at all
  (`on_call`), what the client gets back (`on_result`), how a request's output
  stream is delivered (`wrap_sink`), and what the catalogs advertise
  (`on_list`). Every hook is optional; hooks compose in registration order and
  see the channel's embedder-bound context. See `docs/middleware.md`.
- **Authorization on every decision point** (`on_authorize`, hook ②). Your
  policy now sees all five of the runtime's decision points — `tools/list`
  (per tool), `tools/call`, `resources/list` (per resource),
  `resources/templates/list` (per template) and `resources/read` — with the
  resolved tool identity, the request's whole params (MRTR continuation
  traffic included — visibility, not channel binding), and the per-channel
  context. An undecidable verdict is distinct from a denial: it maps to
  `-32603`, never `-32003`, and fails a whole listing rather than silently
  hiding the entry it could not evaluate.
- **Audit with both views** (`on_audit`, hook ⑤). A journal entry now carries
  both the identity the client asked for and the identity that actually ran,
  so a rename or transform cannot launder what was requested. A denied
  `resources/read` is journalled, as a denied `tools/call` always was.
- **Tool renaming and argument injection** (`on_resolve`, hook ①). You can now
  publish a tool under a different name, hide an argument from clients and supply
  it yourself, or map a whole family of published names onto one real tool — the
  client calls `edit_doc`, the runtime runs `hermes.content.apply` with an API key
  the client never saw. The rename resolves *before* the policy decision, so
  re-exposing an apply-effect tool under a gentler name is still an apply as far as
  `on_authorize` is concerned. Arguments are validated against the real tool's
  schema, so a transform that changes an argument's **type** must convert it here.
- **Catalog transformation** (`on_list`, hook ⑦). You can now rewrite what
  `tools/list`, `resources/list` and `resources/templates/list` return: rename
  entries, rewrite their schemas or descriptions, filter them per principal, or add
  synthetic entries no provider registered — the catalog half of a proxy or a
  retrieval-first meta-tool. It runs after `on_authorize` has filtered, so a denied
  entry cannot be transformed back into view, and a synthetic name still passes
  `on_resolve` and `on_authorize` when it is called.
- **Answering a call without reaching the provider** (`on_call`, hook ③). You can
  now short-circuit a tool call from a middleware — a cache hit, a proxied
  upstream, a canned refusal — and the provider is never invoked. Arguments are
  read-only here by design: rewriting them is `on_resolve`'s job, because arguments
  mutated at this point would be mutated after schema validation. A substituted
  result is validated exactly like a provider's, output schema included.
- **Result rewriting and redaction** (`on_result`, hook ④). You can now redact,
  truncate or rewrite a tool result before it reaches the client. Several hooks
  compose in registration order. The runtime validates the **provider's** result
  against the real output schema before this hook runs; your replacement is
  re-checked for structural and content-block validity but not against that schema,
  because dropping a field the schema requires is what redaction is — keeping
  `on_list`'s advertised `outputSchema` consistent with what you emit is yours to
  do, and the schema is handed to the hook so you can.
- **Decorating a request's outbound frames** (`wrap_sink`, hook ⑥). You can now
  wrap the delivery path a single request's output travels — every
  `notifications/progress` frame it emits and its final response — for progress
  coalescing, stream redaction or per-request accounting. Every member of the
  wrapper is optional and a NULL one forwards untouched. Progress is still
  delivered strictly ahead of the response, and the runtime enforces the two ways a
  wrapper could break a request: a swallowed response is detected and answered past
  the chain with `-32603` rather than left to wedge the connection, and a second
  completion for one id is refused.
- **API break (ABI 2 → 3), migration is one call.**
  `maelys_mcp_runtime_config_t` loses `authorize`, `audit` and
  `policy_context`: the chain replaces those callbacks rather than coexisting
  with them, because two similar-looking mechanisms on the security-critical
  path double the surface that has to be audited. To migrate, call
  `maelys_mcp_runtime_add_compat_policy(runtime, authorize, audit,
  policy_context)` before the first channel — it registers a built-in
  middleware invoking the same callbacks with the same metadata.
  `maelys_mcp_request_context_t`, `maelys_mcp_authorize_fn` and
  `maelys_mcp_audit_fn` moved to `maelys/mcp/middleware.h`, which
  `maelys/mcp.h` includes.
- **Behaviour change: policy runs before schema validation on `tools/call`.**
  A denied caller gets `-32003` with no detail whether or not its arguments
  would have validated, so it can no longer map a tool's argument schema
  through validation error messages. Consequence for middleware authors:
  `on_authorize` sees params the runtime has not validated yet.
- Plumbing for the above, all additive within the unreleased ABI 3:
  `maelys_mcp_middleware_t` gains the five new hook fields (in hook order;
  designated initializers keep compiling); `maelys_mcp_response_sink_t`
  becomes a public opaque type with `maelys_mcp_sink_emit`/`_complete`/
  `_cancelled` to forward through; and the middleware suite runs under
  `make tsan` as well as `make check`, because `wrap_sink` sits on the path
  every request's output takes.

## 0.13.1 - 2026-08-20

- **Fix: Linux release builds compile again.** GCC (used only by the release
  workflow's Ubuntu runners) rejected a too-small error-message buffer in
  `host/manifest.c` under `-Werror=format-truncation=`; clang, used by the
  regular CI checks and macOS builds, never flagged it. No behavioural
  change. Note: v0.13.0's tag was never followed by a GitHub Release (its
  Linux builds failed), so this is the first release actually carrying
  0.13.0's features.

## 0.13.0 - 2026-08-20

- **MCP proxy provider — plug any third-party MCP server into the runtime as
  if it were a native provider.** Point the host at an MCP server executable
  and its tools appear alongside your own, with effect gating, schema
  validation, policy and audit applied to traffic this runtime did not
  originate. The runtime spawns it over stdio, negotiates its protocol era
  (modern `2026-07-28` or a legacy dated revision), and pins its tool catalog
  at connect time — calls resolve only against that snapshot, so a renamed or
  vanished upstream tool cannot be silently substituted. Progress relays end
  to end on the caller's own token. See `docs/mcp-proxy.md`.
- **Choose what happens to upstream tools with unsupported schemas** (proxy
  `schemaPolicy`). One exotic tool no longer has to fail the whole
  connection: `strict` (default) still fails the connect naming the tool,
  `skip` drops just that tool and reports it, `passthrough` exposes it with a
  permissive schema and lets the upstream validate its own arguments —
  effect gating, policy and audit still apply.
- **Declare your whole provider fleet in one JSON file** (`--manifest
  /absolute/path.json`). Native and proxy providers plus `allowEffects`, in
  one declarative document instead of one flag per process. Validation is
  strict and two-phase — any unknown key is rejected by name and location,
  and nothing is constructed from a partially valid document. Composes with
  `--provider`/`--allow-effect`. See `docs/manifest.md`.
- **Progress notifications, end to end.** A long-running tool call can now
  stream `notifications/progress` to the client that opted in via
  `_meta.progressToken`, in strict order ahead of the final response — from
  in-process providers and across the provider wire (negotiated
  `maelys-provider/3` → `/4`; an unmodified `/3` provider keeps working
  untouched). Closes the official `tools-call-with-progress` conformance
  scenario in both the modern and legacy requirement sets.
- **Internal: transport-neutral response sink.** Dispatch delivers through an
  `emit`/`complete`/`cancelled` seam instead of assuming one buffered reply —
  no behavioural change on its own; the foundation progress (and future HTTP
  streaming) builds on.
- **Wider conformance and performance coverage**: the legacy `2025-11-25`
  official requirement set now runs in CI (fetched live, not hand-copied),
  plus a permanent concurrent-dispatch test and channel/provider performance
  baselines on every `make check`/`make tsan`.

## 0.12.2 - 2026-08-17

- **Legacy clients can now use elicitation, sampling and roots.** A client on
  `2024-11-05` through `2025-11-25` that declared the capability once at
  `initialize` gets the same `input_required`/MRTR flow as modern clients, on
  both `tools/call` and `resources/read`. Previously every legacy channel was
  unconditionally denied regardless of what it declared. A legacy channel
  that declared nothing still gets `-32021`/`requiredCapabilities`.

## 0.12.1 - 2026-08-17

- **Fix: legacy clients sending per-request metadata could not start.** A
  request carrying an opaque `_meta` object (a progress token, for example)
  on a legacy-initialized channel was wrongly forced through modern
  negotiation and rejected with `-32602` — observed as a startup failure
  with real legacy clients (e.g. Hermes). Only `_meta` actually carrying
  `io.modelcontextprotocol/protocolVersion` is treated as negotiation now.

## 0.12.0 - 2026-08-17

- **Older MCP protocol versions are accepted at `initialize`.** The runtime
  now negotiates `2024-11-05`, `2025-03-26`, `2025-06-18` and `2025-11-25`
  and echoes the client's requested version back, so clients announcing an
  older dated revision (such as Codex) are no longer rejected with `-32602`.
  Modern `2026-07-28` support is unchanged.

## 0.11.0 - 2026-08-17

- Add a tag-triggered binary release pipeline (`.github/workflows/release.yml`)
  with separated build and publish roles: the privileged publish job runs no
  candidate code, only verifies checksums and attaches artifacts.
- Ship per-platform static and dynamic tarballs (Linux x86_64/arm64, macOS
  arm64/x86_64) via `scripts/package-release.sh`, each with a SHA-256 checksum
  and a build provenance attestation.
- Move `VERSION` into a data file read by both `make` and the release workflow.

## 0.10.0 - 2026-08-15

- Introduce opaque `maelys_mcp_channel_t` handles. Each channel owns its ordered
  output queue, subscriptions, cancellation scope and legacy client lifecycle.
- Replace the runtime-wide writer outbox with passive bounded per-channel queues;
  transports now pump output through deadline-aware `maelys_mcp_channel_next`.
- Route every response, protocol error, listen acknowledgement, provider event and
  graceful completion through the channel queue. A response admission timeout faults
  only that channel; notification coalescence and rejection remain local.
- Make provider event fan-out multi-channel with stable per-channel references and
  local close barriers. Identical JSON-RPC subscription ids on different channels no
  longer collide or permit cross-delivery.
- Make provider activation runtime-scoped, once-only and fail-closed. Make
  `maelys_mcp_runtime_destroy` return `MAELYS_MCP_ERR_STATE` while a channel handle
  remains alive, including a closed handle not yet destroyed.
- Remove `maelys_mcp_runtime_handle`, `maelys_mcp_runtime_attach_outbox` and
  `maelys_mcp_runtime_detach_outbox` without compatibility wrappers. Migrate by
  creating a channel, passing requests to `maelys_mcp_channel_handle`, pumping output
  with `maelys_mcp_channel_next`, then closing and destroying the channel before the
  runtime.
- Increment the native ABI from 1 to 2. The `maelys-provider/3` wire protocol and C,
  TypeScript and Python provider SDK contracts are unchanged.

## 0.9.0 - 2026-08-15

- Bound stdio message writes with a configurable monotonic deadline and wake the
  service loop when the writer fails, preventing a non-draining client from freezing
  the runtime and its producers indefinitely.
- Add the backward-compatible `maelys_mcp_runtime_serve_stdio_with_options` API and
  `--stdio-write-timeout-ms` host option; the existing API uses a five-second default.
- Eliminate an unlocked `writer_status` read in the outbox and defensively reject empty
  subscription URIs before descendant matching.
- Add deterministic randomized coverage for outbox coalescence/byte accounting and the
  subscription state machine, plus an end-to-end stalled-client regression test.
- Keep native ABI version 1: the stdio options record and entry point are additive and
  no released public layout changed.

## 0.8.0 - 2026-08-15

- Add a public C SDK for `maelys-provider/3` process providers, including JSONL
  dispatch, tool and resource callbacks, descriptor validation, and serialized events.
- Isolate provider stdout by default, preserving the protocol stream when callers
  provide only partial serving options.
- Define callback ownership and shutdown semantics so event-producing provider threads
  stop and join before the SDK is destroyed.
- Keep native ABI version 1: the new SDK handle remains opaque and the public additions
  are source- and binary-compatible.

## 0.7.0 - 2026-08-14

- Replace the private process contract with `maelys-provider/3`: an explicit activation
  barrier followed by id-less resource and catalog change events.
- Add one permanent reader thread per process provider, serialize exchanges, enforce
  response deadlines with condition variables, and fail closed on malformed events or
  unexpected response ids.
- Route provider events through URI validation, subscription filtering, causal
  coalescence and the existing single-writer Outbox.
- Extend the TypeScript and Python provider SDKs with activation-aware, serialized,
  thread-safe event facades; update the example and black-box conformance runner.
- Protect `main` with required CI checks and pull requests, enable secret scanning and
  private vulnerability reporting, and add security and contribution policies.
- Keep native ABI version 1: the provider handle remains opaque and the C additions are
  source- and binary-compatible.

## 0.6.2 - 2026-08-14

- Give queued subscription metadata and completion responses independent Jansson ID
  objects, eliminating a producer/writer reference-count race detected by Linux TSan.
- Keep continuous integration on every branch push while avoiding duplicate runs when
  an already-tested commit is subsequently tagged for release.

## 0.6.1 - 2026-08-14

- Add continuous integration for every push and pull request, covering native, SDK,
  provider, analyzer, sanitizer, fuzzer and supported official MCP conformance gates.
- Isolate release, ASan, TSan and libFuzzer artifacts under separate build profiles so
  instrumentation flags can never leak into a normal build.
- Publish the MIT license at the repository root.
- Define native ABI version 1, expose runtime/package version queries, keep all stateful
  public handles opaque and document the compatibility policy through 1.0.

## 0.6.0 - 2026-08-14

- Add an independently enabled MCP 2026-07-28 Subscriptions module implementing
  `subscriptions/listen` with an acknowledged accepted-filter subset.
- Negotiate Tools and Resources list changes plus canonical resource URI subscriptions;
  omit unsupported Prompt filters instead of advertising an unavailable capability.
- Tag every event and graceful completion with
  `io.modelcontextprotocol/subscriptionId`, and cancel active listens through
  `notifications/cancelled` without producing an unsolicited response.
- Route acknowledgements, resource/tool notifications and completion responses through
  the 0.5 single-writer outbox; keyed updates inherit causal coalescence and bounded
  backpressure.
- Add public event-producer APIs, bounded subscription storage, duplicate-id rejection,
  URI normalization, exact/subresource matching and native/ASan/TSan coverage.

## 0.5.0 - 2026-08-14

- Add a reusable bounded asynchronous outbox with one writer thread and explicit
  Jansson ownership transfer.
- Separate responses and notifications, prioritize responses with configurable 8:1
  anti-starvation scheduling, and batch pointer removal outside the I/O critical path.
- Coalesce keyed notifications while moving replacements to their newest causal
  position; bound both message count and serialized byte budget.
- Route every stdio response through the outbox so no runtime worker or module writes
  directly to the protocol descriptor.
- Add deterministic scheduling, coalescence, writer-failure, bounded-backpressure and
  multi-producer tests under native and sanitizer gates.

## 0.4.0 - 2026-08-14

- Add an independently enabled Resources module implementing `resources/list`,
  `resources/templates/list`, and `resources/read` for both supported MCP eras.
- Extend `maelys-provider/2` and the TypeScript/Python provider SDKs with static
  resources, URI templates, reads, and multi-round resource results.
- Introduce the opaque `maelys-uri` security facade over uriparser: exact-length
  input, RFC 3986 normalization, embedded/encoded NUL rejection, and bounded URI
  template validation.
- Validate text/blob exclusivity, MIME fields, base64 payloads, canonical URIs, and
  optional resource sizes before data crosses the provider boundary.
- Add native, process-provider, SDK, sanitizer, analyzer, and URI fuzz coverage.

## 0.3.0 - 2026-08-14

- Replace implicit method dispatch with an explicit module registry and derive
  advertised capabilities from enabled modules.
- Move Tools out of the protocol core and add an independently enabled MRTR module.
- Replace `maelys-provider/1` with the breaking, explicit `maelys-provider/2` result
  contract; remove the legacy arbitrary-JSON call result.
- Support structured content alongside text, image, audio, resource-link and embedded
  resource blocks with bounded shape, MIME and base64 validation.
- Forward client capabilities, `inputResponses`, and opaque `requestState` across
  provider rounds; reject undeclared required capabilities with MCP error `-32021`.
- Extend native, TypeScript, Python, provider-conformance and official MCP coverage.
- Make provider describe, call and shutdown deadlines independently configurable;
  bound process termination with `SIGTERM` then `SIGKILL` escalation.
- Buffer JSON Lines without losing co-read frames and protect every provider socket
  with close-on-exec before any subsequent provider is launched.
- Reject embedded NUL bytes in protocol discriminants, MIME, base64, URI and opaque
  state fields before they cross a length-unaware C boundary.

## 0.2.0 - 2026-08-14

- Allow provider launchers using `#!/usr/bin/env node` or Python to resolve their
  interpreter from a fixed cross-platform system `PATH` without inheriting the caller
  environment.
- Add dependency-free TypeScript and Python provider SDK packages with descriptor and
  schema validation aligned to the native runtime.
- Add a black-box provider conformance runner covering persistent lifecycle, scenarios,
  structured errors, shutdown and stdout contamination.
- Document stdio client setup for Codex and Claude Code.
- Add a pinned, explicitly partial integration with the official MCP 2026-07-28
  conformance runner for the supported tools-only facade.
- Clarify that TypeScript and Python packages implement only the private provider
  protocol and are not general MCP SDKs.

## 0.1.0 - 2026-08-14

- Add the provider-neutral C runtime and stdio host.
- Support MCP 2026-07-28 and compatibility with MCP 2025-11-25.
- Add persistent external providers with policy effect classes.
- Isolate protocol stdout from library and diagnostic output.
- Reject schema features that the runtime cannot enforce.
- Port generic JSON-RPC, MCP lifecycle, provider, and stdio regression coverage.
- Add digest-pinned Linux ASan/UBSan/LSan validation and three libFuzzer targets.
