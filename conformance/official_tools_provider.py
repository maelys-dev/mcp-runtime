#!/usr/bin/env python3
"""Fixture provider for the supported official MCP tools conformance scenarios."""

from pathlib import Path
import sys


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sdk" / "python" / "src"))

from maelys_mcp_provider import (
    Tool, complete_result, create_provider, input_required_result, serve_provider,
)


EMPTY_OBJECT_SCHEMA = {
    "type": "object",
    "properties": {},
    "additionalProperties": False,
}


PNG = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Wl2nXsAAAAASUVORK5CYII="
WAV = "UklGRgQAAABXQVZF"


def simple_text(_arguments: dict[str, object], _context: object):
    return complete_result(content=[{
        "type": "text", "text": "This is a simple text response for testing.",
    }])


def image_content(_arguments: dict[str, object], _context: object):
    return complete_result(content=[{"type": "image", "data": PNG, "mimeType": "image/png"}])


def audio_content(_arguments: dict[str, object], _context: object):
    return complete_result(content=[{"type": "audio", "data": WAV, "mimeType": "audio/wav"}])


def embedded_resource(_arguments: dict[str, object], _context: object):
    return complete_result(content=[{"type": "resource", "resource": {
        "uri": "test://embedded-resource", "mimeType": "text/plain",
        "text": "Embedded resource content for testing.",
    }}])


def mixed_content(_arguments: dict[str, object], _context: object):
    return complete_result(content=[
        {"type": "text", "text": "Multiple content types test:"},
        {"type": "image", "data": PNG, "mimeType": "image/png"},
        {"type": "resource", "resource": {
            "uri": "test://mixed-content-resource", "mimeType": "application/json",
            "text": '{"test":"data","value":123}',
        }},
    ])


def input_required_elicitation(_arguments: dict[str, object], context: object):
    responses = getattr(context, "input_responses", None)
    user_name = responses.get("user_name") if isinstance(responses, dict) else None
    if isinstance(user_name, dict):
        content = user_name.get("content")
        name = content.get("name") if isinstance(content, dict) else "user"
        return complete_result(content=[{"type": "text", "text": f"Hello, {name}!"}])
    return input_required_result(input_requests={"user_name": {
        "method": "elicitation/create",
        "params": {"message": "What is your name?", "requestedSchema": {
            "type": "object", "properties": {"name": {"type": "string"}},
            "required": ["name"],
        }},
    }})


def capability_aware_input(_arguments: dict[str, object], context: object):
    capabilities = getattr(context, "client_capabilities", None)
    requests = {}
    if isinstance(capabilities, dict) and isinstance(capabilities.get("sampling"), dict):
        requests["capital_question"] = {
            "method": "sampling/createMessage",
            "params": {"messages": [{"role": "user", "content": {
                "type": "text", "text": "What is the capital of France?",
            }}], "maxTokens": 100},
        }
    if isinstance(capabilities, dict) and isinstance(capabilities.get("elicitation"), dict):
        requests["user_name"] = {
            "method": "elicitation/create",
            "params": {"message": "What is your name?", "requestedSchema": {
                "type": "object", "properties": {"name": {"type": "string"}},
            }},
        }
    return input_required_result(input_requests=requests)


def intentional_error(_arguments: dict[str, object], _context: object) -> object:
    raise RuntimeError("This tool intentionally returns an error for testing")


PROVIDER = create_provider(
    "official-conformance",
    "0.3.0",
    (
        Tool(
            name="test_simple_text",
            title="Return simple text",
            description="Fixture used by the official MCP simple text scenario.",
            input_schema=EMPTY_OBJECT_SCHEMA,
            effect="read",
            handler=simple_text,
        ),
        Tool(name="test_image_content", title="Return image", description="Image fixture.",
            input_schema=EMPTY_OBJECT_SCHEMA, effect="read", handler=image_content),
        Tool(name="test_audio_content", title="Return audio", description="Audio fixture.",
            input_schema=EMPTY_OBJECT_SCHEMA, effect="read", handler=audio_content),
        Tool(name="test_embedded_resource", title="Return resource", description="Resource fixture.",
            input_schema=EMPTY_OBJECT_SCHEMA, effect="read", handler=embedded_resource),
        Tool(name="test_multiple_content_types", title="Return mixed content",
            description="Mixed content fixture.", input_schema=EMPTY_OBJECT_SCHEMA,
            effect="read", handler=mixed_content),
        Tool(name="test_input_required_result_elicitation", title="Request a name",
            description="MRTR elicitation fixture.", input_schema=EMPTY_OBJECT_SCHEMA,
            effect="read", handler=input_required_elicitation),
        Tool(name="test_input_required_result_capabilities", title="Respect capabilities",
            description="Capability-aware MRTR fixture.", input_schema=EMPTY_OBJECT_SCHEMA,
            effect="read", handler=capability_aware_input),
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
