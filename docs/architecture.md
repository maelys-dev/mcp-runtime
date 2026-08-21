# Architecture

## Purpose

The runtime is a protocol and policy boundary shared by Maelys, projectctl, Hermes,
and future developer tools. Each tool keeps its business logic and native language.

```text
MCP client
    |
    v
maelys-mcp host
    |- JSON-RPC and MCP validation
    |- middleware chain (policy and audit hooks)
    |- channel registry and provider-event fan-out
    |- module registry
    |   |- Tools
    |   |- Resources
    |   |- Subscriptions
    |   `- MRTR
    |
    +--> channel A -> passive bounded outbox -> transport pump
    +--> channel B -> passive bounded outbox -> transport pump
    +--> in-process C provider
    +--> persistent Python provider
    +--> persistent TypeScript provider
```

## Dependency rule

The runtime must not include headers or source files from codexmanager, projectctl,
Hermes, or any provider. Dependencies point toward the public provider contract, never
toward application implementations.

The C, TypeScript and Python SDK packages implement that public contract but contain
no application tools. Providers depend on an SDK; the native runtime never depends on
an application package.

Concrete providers live with the application that owns their business rules. For
example, the Hermes provider belongs in the Hermes repository; this repository keeps
only the protocol, SDKs, reference provider and conformance fixtures. That boundary
lets Hermes release its tools independently without coupling the runtime to editorial
code.

Codex and Claude are MCP clients, not provider implementations. Their integration is a
small launch configuration that starts the same `maelys-mcp` stdio host; no compiled
client-specific adapter belongs in this library.

## Ownership

- `maelys_mcp_runtime_add_provider` transfers provider ownership on success.
- The caller retains ownership when registration fails.
- Provider descriptors are deep-copied by `maelys_mcp_provider_create`.
- Provider callbacks fill one initialized `maelys_mcp_provider_result_t`. Every JSON
  field placed in it is a newly owned Jansson reference. The runtime releases all
  fields with `maelys_mcp_provider_result_clear`.
- `maelys_mcp_channel_handle` borrows the request. It returns an operational status;
  every protocol response is transferred to the channel outbox instead of being
  returned directly.
- Resource descriptors are deep-copied and normalized at registration. Resource read
  callbacks follow the same ownership rule through `maelys_mcp_resource_result_t`.
- `maelys_mcp_outbox_enqueue_take` steals a Jansson reference only on success. The
  consumer returned by `maelys_mcp_outbox_next` owns that reference. Producers never
  write to a protocol descriptor.
- A channel owns its outbox, subscription ids, resource filters and legacy client
  state. The runtime retains the channel's lifetime entry until `channel_destroy`,
  even after `channel_close` has removed it from event fan-out.
- `maelys_mcp_channel_destroy` consumes the handle even when its bounded close
  missed the deadline, and then waits, without a bound, for every operation that
  had already retained the channel: freeing at the deadline while workers still
  hold the channel's mutex and outbox would be a use-after-free.
  `maelys_mcp_channel_destroy_detached` makes the same guarantee without the
  wait. It aborts the channel, unlinks it from the registry immediately, moves
  it onto a detached ledger the runtime tracks separately from
  `live_channel_count`, and returns; whichever operation finishes last performs
  the real free on its own thread, and a detached channel's worker disposes of
  its own thread handle because the thread that would have joined it has
  already returned. `maelys_mcp_runtime_destroy` drains that ledger before it
  frees anything, so a runtime is never released under a channel that still
  points at it. The embedder-bound channel context can therefore outlive the
  detaching call, and is only certainly unreachable once
  `maelys_mcp_runtime_destroy` has returned.
- `maelys_mcp_provider_emit_event` borrows the event payload only for the duration of
  the call. Process providers activate once when the first channel is created and
  remain active until runtime destruction.

## Policy boundary

Policy, audit and transformation are a chain of middleware, registered on the
runtime while it is cold and immutable from its first channel onward, so dispatch
reads it with no lock. Seven hooks are implemented, in the order one request meets
them: `on_resolve` maps the client's tool name and arguments onto the real ones;
`on_authorize` is consulted at all five decision points (`tools/list` per tool,
`tools/call`, `resources/list` per resource, `resources/templates/list` per
template, `resources/read`); `on_call` invokes or substitutes with read-only
arguments; `on_result` rewrites or redacts; `on_audit` journals `tools/call` and
`resources/read` including their denials. Around all of them, `wrap_sink`
decorates the request's outbound delivery path, and `on_list` transforms a
catalog after `on_authorize` has filtered it. Every hook — a `wrap_sink`
wrapper's three functions included — runs on the request thread with no runtime
lock held, so the chain does not extend the lock hierarchy; a wrapper's
per-request state lives on that thread's stack frame and is never shared.

The runtime holds no policy vocabulary of its own. A decision is taken on the
resolved identity - the tool the runtime will actually invoke, the canonical URI
it will actually read - and on the opaque, embedder-owned pointer bound to the
channel, which the runtime carries and never interprets. It is never taken on
`clientInfo.name`, which is client-asserted in both protocol eras. On
`tools/call` the decision precedes schema validation, so a denied caller cannot
probe argument schemas through validation error details, and the hook sees the
request's whole params, `inputResponses` and `requestState` included, so MRTR
continuation traffic is not invisible to policy.

Transformation is ordered so that it can never become privilege: `on_resolve`
runs before the decision, so a rename resolves to the real tool and the real
effect before anything is allowed, and `on_list` runs after it, so a denied
catalog entry cannot be transformed back into view. Arguments are validated
against the real tool's schema, on `on_resolve`'s output. See
`docs/middleware.md`.

## Protocol eras

The same dispatcher supports:

- modern MCP `2026-07-28`, with per-request metadata and no session;
- legacy MCP `2025-11-25`, with `initialize` for compatible clients.

Modern behavior is selected by
`params._meta["io.modelcontextprotocol/protocolVersion"]`.

Which of the two a given connection is offered is a property of the **channel**,
not of the transport and not of the runtime: one runtime can serve a channel
that speaks both and a channel that speaks only `2026-07-28` at the same time,
and no transport ever rewrites a dispatch result to make that true. Every
channel starts serving both, and `maelys_mcp_channel_set_protocol_eras` narrows
it. The mask has exactly three effects, all inside the dispatcher:
`server/discover` announces only the eras still set; `initialize` is refused
with `-32600` once the legacy era is cleared; and a request carrying modern
`_meta` negotiation is refused with `-32022` once the modern era is cleared,
naming what the channel does serve. A negotiated legacy session cannot be
withdrawn afterwards - the setter refuses that, rather than stranding a client
mid-session.
Modern final results carry `resultType: "complete"`, server identity metadata, and
conservative cache hints on cacheable list/discovery operations. When the MRTR module
is enabled, a provider may instead return `input_required`; the retry's
`inputResponses`, opaque `requestState`, and client capabilities are passed back to
the same provider.

## Nested requests

MRTR has two shapes, and the runtime serves both. The resumable one is
`input_required` above: the call ends, and the client retries. The nested one keeps
the call open - the runtime sends a real server-to-client request
(`elicitation/create`, `sampling/createMessage` or `roots/list`) on the same
connection and blocks the call until the client answers. Older clients understand
only the nested shape, because `resultType`/`inputRequests` is a `2026-07-28` draft
type that a schema-validating `2025-11-25` client rejects.

Correlation lives at the channel, not in a transport: each channel keeps a table of
outstanding host-to-client requests keyed by a host-generated
`maelys/nested/<n>` id (string-prefixed, so it can never collide with a
client-chosen one), guarded by the channel mutex and woken by its own condition.
`maelys_mcp_channel_accept` is the seam a transport calls: it matches an inbound
frame against that table before dispatching anything, offloads the two nestable
methods, and dispatches everything else inline. `stdio.c` is a thin adapter over
it, and a future HTTP transport reuses it unchanged.

The nested request travels the request's own response sink, so it stays ordered
ahead of the final result and remains visible to a `wrap_sink` middleware. Its
deadline is separate from the provider call deadline, which is suspended while it
is outstanding; a person answering an elicitation is not the provider being slow.
The wait is settled - never left standing - by the client's reply, by a
`notifications/cancelled` naming the outer call, by the channel faulting or
closing, by the provider dying, or by that deadline. Providers reach it through
`maelys_mcp_provider_request_client`; a NULL relay means this dispatch cannot
carry one, and the provider falls back to `input_required`. See
`docs/provider-protocol.md` for the wire.

The official `2025-11-25` conformance requirements test exactly this shape, not
the resumable one: `tools-call-sampling`, `tools-call-elicitation`,
`elicitation-sep1034-defaults` and `elicitation-sep1330-enums` each drive a real
client that answers a server-initiated request mid-`tools/call` and expects the
call to complete once it does. `conformance/official_tools_provider.py`'s
`test_sampling`/`test_elicitation`/`test_elicitation_sep1034_defaults`/
`test_elicitation_sep1330_enums` handlers exercise this path through the Python
SDK's `context.request_sampling`/`context.request_elicitation`, and
`docs/official-conformance.md` records the resulting scenario coverage.

## Module boundary

The core owns lifecycle, version negotiation, discovery and generic JSON-RPC routing.
It does not contain method-name branches for `tools/list` or `tools/call`. Active
modules publish their capabilities and claim their methods through the internal
registry. A newly created runtime has no application capability; the host explicitly
enables Tools, Resources, MRTR and Subscriptions. A provider may expose tools,
resources, or both;
registration fails unless every required module is enabled.

MRTR deliberately remains separate from Tools even though the first supported use is
`tools/call`: Tools owns catalog and execution semantics; MRTR owns permission for
multi-round results and retry fields. Future Prompts or Resources modules can reuse
MRTR without copying it. Resources owns catalog, template and read semantics, while
the opaque `maelys-uri` facade owns bounded parsing and normalization.

Subscriptions owns long-lived listen requests and accepted event filters per channel, but no
provider business logic. Enabling it makes the Tools `listChanged` and Resources
`listChanged`/`subscribe` capability flags true. Producers publish semantic events
through the public runtime API. Fan-out snapshots targetable channels under the runtime
registry lock and retains a local operation reference for each. Each channel then
snapshots matching subscription ids under its own mutex and releases all locks before
constructing or enqueueing messages.

## Current JSON Schema subset

The runtime validates `type`, `properties`, `required`, `additionalProperties`,
`items`, `enum`, `minLength`, `maxLength`, `minimum`, and `maximum`. Providers remain
responsible for their domain invariants. `$schema`, `title`, and `description` are
accepted as annotations. Every schema node must declare one supported `type`; tool
input schemas must have an object root. Unsupported validation keywords and malformed
definitions are rejected before a provider is registered, so the schema exposed by
`tools/list` cannot promise a constraint that runtime validation silently ignores.
Full JSON Schema 2020-12 is a later milestone.

## Output boundary

Each channel outbox has separate response and notification queues, bounded by message
count and serialized bytes. `maelys_mcp_outbox_next` removes one message under the
mutex and transfers it to a transport-owned pump; no callback or I/O belongs to the
outbox.
Responses are preferred, but after eight consecutive responses one pending
notification is selected. Keyed notification replacement moves the event to the tail,
preserving the most recent causal position rather than its first occurrence.

The queues accept multiple producers. Native and process-provider event APIs are
asynchronous with respect to protocol writes. Request dispatch is synchronous with
exactly two exceptions, `tools/call` and `resources/read`: a transport that delivers
through `maelys_mcp_channel_accept` hands those two to a short-lived worker thread and
returns to reading, because they are the only methods that can end up blocked on a
client round trip (see "Nested requests" below), and the thread that must read that
round trip's reply cannot be the thread waiting for it. Every other method - the
handshake, the list operations, `subscriptions/listen`, notifications - still
dispatches inline on the caller's thread, and `maelys_mcp_channel_handle` is
synchronous for all of them, unchanged. Provider calls remain synchronous from the
caller's point of view and strictly single-outstanding per provider process.
A slow channel's bounded admission cannot indefinitely block
another channel, and fan-out continues after a local admission failure.
The lock hierarchy is provider lifecycle, then runtime channel registry, then channel;
the global provider-callback inflight gate is never nested. A provider's private event
mutex stays held while its sink callback runs so sink detachment cannot race callback
entry; it precedes the inflight gate and is never acquired from that gate. One further
edge exists in one direction only: a process provider's state mutex may precede a
channel mutex, so that a provider's death can cancel the nested wait a worker is
blocked on; nothing takes a channel mutex before a provider state mutex.
A detached channel's real free runs on whichever thread released its last
operation reference and takes the registry lock to retire it from the detached
ledger, but only after releasing the channel mutex, so it walks the same
hierarchy downwards as everything else rather than closing a cycle back up it.
Each process provider has
one reader thread that separates id-less events and nested requests from the one
serialized request/response exchange. A mandatory subscription acknowledgement uses the priority lane so a
later completion response cannot overtake the stream's first message.

Creating a channel or outbox creates no thread. The stdio transport owns one writer
thread for its durable channel. Future transports can use an event loop or shared pool
without changing the channel contract.
