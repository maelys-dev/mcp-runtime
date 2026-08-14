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

Every matching client receives its own notification tagged with
`_meta["io.modelcontextprotocol/subscriptionId"]`. A resource subscription matches its
canonical URI and descendant paths. The coalescence key contains the subscription id,
event kind and URI, so duplicate invalidations for one stream do not suppress another
stream's event.

## Lifecycle and concurrency

The registry is bounded by `runtime_config.max_subscriptions` (64 by default). Duplicate
active ids are rejected. `notifications/cancelled` removes the matching listen without
creating a response. Graceful transport EOF calls
`maelys_mcp_runtime_complete_subscriptions`, which emits one final
`resultType: "complete"` response per remaining id.

The runtime borrows its Outbox. An embedding transport must attach it before accepting
listen requests, stop event producers, detach it, and only then destroy it. The
subscription mutex protects registry snapshots only: no JSON construction, queue wait,
provider callback or protocol I/O runs under that mutex.
