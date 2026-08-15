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
    |- policy and audit hooks
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
- `maelys_mcp_provider_emit_event` borrows the event payload only for the duration of
  the call. Process providers activate once when the first channel is created and
  remain active until runtime destruction.

## Protocol eras

The same dispatcher supports:

- modern MCP `2026-07-28`, with per-request metadata and no session;
- legacy MCP `2025-11-25`, with `initialize` for compatible clients.

Modern behavior is selected by
`params._meta["io.modelcontextprotocol/protocolVersion"]`.
Modern final results carry `resultType: "complete"`, server identity metadata, and
conservative cache hints on cacheable list/discovery operations. When the MRTR module
is enabled, a provider may instead return `input_required`; the retry's
`inputResponses`, opaque `requestState`, and client capabilities are passed back to
the same provider.

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
asynchronous with respect to protocol writes, while request dispatch and provider calls
remain synchronous. A slow channel's bounded admission cannot indefinitely block
another channel, and fan-out continues after a local admission failure.
The lock hierarchy is provider lifecycle, then runtime channel registry, then channel;
the global provider-callback inflight gate is never nested. A provider's private event
mutex stays held while its sink callback runs so sink detachment cannot race callback
entry; it precedes the inflight gate and is never acquired from that gate. Each process provider has
one reader thread that separates id-less events from the one serialized
request/response exchange. A mandatory subscription acknowledgement uses the priority lane so a
later completion response cannot overtake the stream's first message.

Creating a channel or outbox creates no thread. The stdio transport owns one writer
thread for its durable channel. Future transports can use an event loop or shared pool
without changing the channel contract.
