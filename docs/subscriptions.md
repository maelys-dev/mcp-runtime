# MCP subscriptions

The optional Subscriptions module implements the long-lived MCP 2026-07-28
`subscriptions/listen` request for capabilities the runtime actually exposes.

## Negotiation

The client sends a normal modern request with a stable string or integer id and a
`notifications` filter. The runtime validates all declared boolean fields, canonicalizes
resource URIs through `maelys-uri`, removes duplicates, and acknowledges only the
supported subset:

- `toolsListChanged` when Tools is enabled;
- `resourcesListChanged` when Resources is enabled;
- `resourceSubscriptions` when Resources is enabled;
- no `promptsListChanged` until a Prompts module exists.

The first output for the listen id is
`notifications/subscriptions/acknowledged`. The listen request then remains open; it
does not receive an immediate JSON-RPC result.

## Event API

Embedding applications publish semantic changes without constructing protocol JSON:

```c
maelys_mcp_runtime_notify_resource_updated(runtime, "hermes://repo/course.mdx");
maelys_mcp_runtime_notify_resources_list_changed(runtime);
maelys_mcp_runtime_notify_tools_list_changed(runtime);
```

An opaque in-process provider may call `maelys_mcp_provider_emit_event`. Process
providers use the equivalent activation-gated `maelys-provider/3` notifications; the
runtime validates them and enters this same API path.

Every matching client receives its own notification tagged with
`_meta["io.modelcontextprotocol/subscriptionId"]`. A resource subscription matches its
canonical URI and descendant paths. The coalescence key contains the subscription id,
event kind and URI, so duplicate invalidations for one stream do not suppress another
stream's event.

## Lifecycle and concurrency

Each channel registry is bounded by `runtime_config.max_subscriptions` (64 by
default). Duplicate active ids are rejected only within that channel. The same id may
be active on another channel without collision. `notifications/cancelled` removes a
matching listen exclusively from the receiving channel.

Graceful channel close emits one final `resultType: "complete"` response per remaining
id, closes admission, and waits for the transport to drain those responses within the
close deadline. A failed admission faults only that channel and does not claim that an
unadmitted completion was delivered.

Provider events snapshot targetable channels and retain a local operation reference
before releasing the runtime registry lock. Each channel then snapshots subscriptions
under its own mutex. No JSON construction, queue wait, callback or I/O runs under the
runtime registry or channel mutex. Closing a channel waits only for references already
retained for that channel; it never stops fan-out to peers.
