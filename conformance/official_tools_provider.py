#!/usr/bin/env python3
"""Fixture provider for the supported official MCP tools conformance scenarios."""

from pathlib import Path
import sys


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sdk" / "python" / "src"))

from maelys_mcp_provider import Tool, create_provider, serve_provider


EMPTY_OBJECT_SCHEMA = {
    "type": "object",
    "properties": {},
    "additionalProperties": False,
}


def simple_text(_arguments: dict[str, object]) -> dict[str, str]:
    return {"message": "This is a simple text response for testing."}


def intentional_error(_arguments: dict[str, object]) -> object:
    raise RuntimeError("This tool intentionally returns an error for testing")


PROVIDER = create_provider(
    "official-conformance",
    "0.2.0",
    (
        Tool(
            name="test_simple_text",
            title="Return simple text",
            description="Fixture used by the official MCP simple text scenario.",
            input_schema=EMPTY_OBJECT_SCHEMA,
            effect="read",
            handler=simple_text,
        ),
        Tool(
            name="test_error_handling",
            title="Return a tool error",
            description="Fixture used by the official MCP tool error scenario.",
            input_schema=EMPTY_OBJECT_SCHEMA,
            effect="read",
            handler=intentional_error,
        ),
    ),
)


if __name__ == "__main__":
    raise SystemExit(serve_provider(PROVIDER))
