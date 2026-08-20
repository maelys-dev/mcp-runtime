# `maelys-mcp-provider-sdk`

Dependency-free Python SDK for persistent `maelys-provider` providers, up to `/5`. It
validates tool descriptors, owns the JSON Lines loop and converts every boundary failure
into a structured provider error.

```python
from maelys_mcp_provider import Tool, complete_result, create_provider, serve_provider

provider = create_provider("example", "1.0.0", [Tool(
    name="example.inspect",
    description="Inspect a resource without mutation.",
    input_schema={"type": "object", "additionalProperties": False},
    output_schema={"type": "object"},
    effect="read",
    handler=lambda arguments, context: complete_result(
        structured_content={"ready": True}),
)])

raise SystemExit(serve_provider(provider))
```

The default server duplicates the protocol descriptor and redirects process-wide
stdout to stderr before reading requests. Third-party `print()` calls therefore cannot
contaminate the provider transport.

Handlers receive `(arguments, context)`. Return `complete_result(...)` or
`input_required_result(...)`; arbitrary JSON results are intentionally rejected.

After host activation, any thread may publish through the SDK's serialized writer:

```python
provider.events.resource_updated("hermes://repository/course.mdx")
provider.events.resources_list_changed()
provider.events.tools_list_changed()
```

Calling an event method before activation or after shutdown fails explicitly.

## Nested client requests

A handler may open one request back at the *client* in the middle of a call and block
for the answer, which is what a schema-validating `2025-11-25` client understands:

```python
def apply_plan(arguments, context):
    answer = context.request_elicitation({
        "message": "Apply these changes?",
        "requestedSchema": {"type": "object",
            "properties": {"accept": {"type": "boolean"}}, "required": ["accept"]},
    })
    if not answer.get("content", {}).get("accept"):
        return complete_result(content=[{"type": "text", "text": "Cancelled."}])
    ...
```

`request_sampling(params)` and `request_roots()` are the other two surfaces the host
will forward; nothing else is allowed, and the client must have declared the matching
capability. This is the counterpart of `input_required_result`, not a replacement:
that one ends the call and lets the client retry, this one keeps the call open.

A refusal raises rather than returning a sentinel. `NestedRequestDenied`,
`NestedRequestTimeout`, `NestedRequestCancelled`, `NestedRequestUnavailable` and
`NestedTransportError` all derive from `NestedRequestError`, which carries the host's
own `code` and, for a client-side failure, the client's error object as `data`:

```python
try:
    answer = context.request_sampling({"messages": [...], "maxTokens": 100})
except NestedRequestDenied:
    return complete_result(content=[{"type": "text", "text": "No model available."}])
```

Call these from the thread handling the call, and only while it is running: the host
fails the whole provider transport for a nested request that arrives while no call is
in flight, so a stashed context raises `NestedRequestUnavailable` here instead. One
request may be outstanding at a time.

Nesting is the only thing that makes this SDK declare `maelys-provider/5`. A provider
that never opens one keeps declaring `/4` and keeps working against a host that
predates nested requests.
