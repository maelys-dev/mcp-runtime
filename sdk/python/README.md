# `maelys-mcp-provider-sdk`

Dependency-free Python SDK for persistent `maelys-provider/2` providers. It validates
tool descriptors, owns the JSON Lines loop and converts every boundary failure into a
structured provider error.

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
