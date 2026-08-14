#!/usr/bin/env python3
from maelys_mcp_provider import Tool, create_provider, serve_provider


def echo(arguments):
    print("simulated third-party diagnostic")
    if not isinstance(arguments.get("message"), str):
        raise ValueError("message is required")
    return {"message": arguments["message"]}


provider = create_provider("python-fixture", "1.0.0", [Tool(
    name="fixture.echo",
    description="Echo a message while a dependency writes a diagnostic.",
    input_schema={
        "type": "object",
        "properties": {"message": {"type": "string", "minLength": 1}},
        "required": ["message"],
        "additionalProperties": False,
    },
    output_schema={"type": "object"},
    effect="read",
    handler=echo,
)])

raise SystemExit(serve_provider(provider))
