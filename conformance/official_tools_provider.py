#!/usr/bin/env python3
"""Fixture provider for the supported official MCP tools conformance scenarios."""

from pathlib import Path
import sys
import time


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sdk" / "python" / "src"))

from maelys_mcp_provider import (
    ProviderNotFoundError, Resource, ResourceTemplate, Tool, complete_result,
    create_provider, input_required_result, resource_result, serve_provider,
)


EMPTY_OBJECT_SCHEMA = {
    "type": "object",
    "properties": {},
    "additionalProperties": False,
}

SAMPLING_INPUT_SCHEMA = {
    "type": "object",
    "properties": {"prompt": {"type": "string"}},
    "required": ["prompt"],
    "additionalProperties": False,
}

ELICITATION_INPUT_SCHEMA = {
    "type": "object",
    "properties": {"message": {"type": "string"}},
    "required": ["message"],
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


def sampling_tool(arguments: dict[str, object], context: object):
    """Fixture for the official `tools-call-sampling` scenario: opens a real
    NESTED sampling/createMessage request mid-call and blocks for the
    client's reply, rather than mcp-runtime's other MRTR shape (ending the
    call with an input_required result the caller retries) - the official
    scenario's own client handler answers synchronously on the same
    connection and expects the tool call to complete once it does, which
    only the nested pattern satisfies. See docs/architecture.md's MRTR
    section for both shapes.
    """
    prompt = arguments.get("prompt") if isinstance(arguments, dict) else None
    prompt = prompt if isinstance(prompt, str) else "Test prompt for sampling"
    result = context.request_sampling({
        "messages": [{"role": "user", "content": {"type": "text", "text": prompt}}],
        "maxTokens": 100,
    })
    content = result.get("content") if isinstance(result, dict) else None
    text = content.get("text") if isinstance(content, dict) else None
    return complete_result(content=[{
        "type": "text", "text": text if isinstance(text, str) else "sampling response",
    }])


def elicitation_tool(arguments: dict[str, object], context: object):
    """Fixture for the official `tools-call-elicitation` scenario: same
    nested pattern as sampling_tool above, but elicitation/create."""
    message = arguments.get("message") if isinstance(arguments, dict) else None
    message = message if isinstance(message, str) else "Please provide your information"
    result = context.request_elicitation({
        "message": message,
        "requestedSchema": {
            "type": "object",
            "properties": {
                "username": {"type": "string"},
                "email": {"type": "string"},
            },
            "required": ["username", "email"],
        },
    })
    content = result.get("content") if isinstance(result, dict) else None
    username = content.get("username") if isinstance(content, dict) else None
    return complete_result(content=[{
        "type": "text",
        "text": f"Hello, {username}!" if isinstance(username, str) else "no response",
    }])


def elicitation_sep1034_defaults(_arguments: dict[str, object], context: object):
    """Fixture for `elicitation-sep1034-defaults`: the official scenario
    inspects the requestedSchema this handler sends - not the client's
    answer - so every property below carries the exact type/default pair
    SEP-1034 requires (one of each JSON Schema primitive type)."""
    context.request_elicitation({
        "message": "Please provide your details",
        "requestedSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "default": "John Doe"},
                "age": {"type": "integer", "default": 30},
                "score": {"type": "number", "default": 95.5},
                "status": {
                    "type": "string", "enum": ["active", "inactive"], "default": "active",
                },
                "verified": {"type": "boolean", "default": True},
            },
        },
    })
    return complete_result(content=[{"type": "text", "text": "elicitation-sep1034-defaults-ok"}])


def elicitation_sep1330_enums(_arguments: dict[str, object], context: object):
    """Fixture for `elicitation-sep1330-enums`: same shape as the SEP-1034
    fixture above, but exercising SEP-1330's five enum-schema forms (plain
    enum, titled oneOf, legacy enumNames, and both as array items)."""
    context.request_elicitation({
        "message": "Please choose",
        "requestedSchema": {
            "type": "object",
            "properties": {
                "untitledSingle": {"type": "string", "enum": ["option1", "option2"]},
                "titledSingle": {
                    "type": "string",
                    "oneOf": [
                        {"const": "value1", "title": "Value 1"},
                        {"const": "value2", "title": "Value 2"},
                    ],
                },
                "legacyEnum": {
                    "type": "string",
                    "enum": ["opt1", "opt2"],
                    "enumNames": ["Option 1", "Option 2"],
                },
                "untitledMulti": {
                    "type": "array",
                    "items": {"type": "string", "enum": ["option1", "option2"]},
                },
                "titledMulti": {
                    "type": "array",
                    "items": {
                        "anyOf": [
                            {"const": "value1", "title": "Value 1"},
                            {"const": "value2", "title": "Value 2"},
                        ],
                    },
                },
            },
        },
    })
    return complete_result(content=[{"type": "text", "text": "elicitation-sep1330-enums-ok"}])


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


def tool_with_progress(_arguments: dict[str, object], _context: object):
    """The official scenario expects 0/100, 50/100 and 100/100 with roughly
    50ms between them.

    It phrases this as "if _meta.progressToken is provided", but a provider
    never sees the token: the host holds it and drops progress when the client
    asked for none. Reporting unconditionally is therefore both correct and
    what the scenario observes, since the client only sees frames when it
    supplied a token.
    """
    for step in (0.0, 50.0, 100.0):
        PROVIDER.events.report_progress(step, 100.0)
        time.sleep(0.05)
    return complete_result(content=[{
        "type": "text", "text": "Progress test completed",
    }])


def intentional_error(_arguments: dict[str, object], _context: object) -> object:
    raise RuntimeError("This tool intentionally returns an error for testing")


def read_resource(uri: str, _context: object):
    if uri == "test://static-text":
        return resource_result([{
            "uri": uri, "mimeType": "text/plain",
            "text": "This is the content of the static text resource.",
        }])
    if uri == "test://static-binary":
        return resource_result([{"uri": uri, "mimeType": "image/png", "blob": PNG}])
    if uri.startswith("test://template/") and uri.endswith("/data"):
        parameter = uri.removeprefix("test://template/").removesuffix("/data")
        return resource_result([{
            "uri": uri, "mimeType": "text/plain", "text": f"Template value: {parameter}",
        }])
    raise ProviderNotFoundError(f"resource not found: {uri}")


PROVIDER = create_provider(
    "official-conformance",
    "0.4.0",
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
        Tool(name="test_tool_with_progress", title="Report progress",
            description="Reports three progress steps before completing.",
            input_schema=EMPTY_OBJECT_SCHEMA, effect="read",
            handler=tool_with_progress),
        Tool(
            name="test_error_handling",
            title="Return a tool error",
            description="Fixture used by the official MCP tool error scenario.",
            input_schema=EMPTY_OBJECT_SCHEMA,
            effect="read",
            handler=intentional_error,
        ),
        Tool(name="test_sampling", title="Request LLM sampling",
            description="Opens a nested sampling/createMessage request mid-call.",
            input_schema=SAMPLING_INPUT_SCHEMA, effect="read", handler=sampling_tool),
        Tool(name="test_elicitation", title="Request user input",
            description="Opens a nested elicitation/create request mid-call.",
            input_schema=ELICITATION_INPUT_SCHEMA, effect="read", handler=elicitation_tool),
        Tool(name="test_elicitation_sep1034_defaults", title="Elicit with schema defaults",
            description="Nested elicitation/create exercising SEP-1034 default values.",
            input_schema=EMPTY_OBJECT_SCHEMA, effect="read",
            handler=elicitation_sep1034_defaults),
        Tool(name="test_elicitation_sep1330_enums", title="Elicit with enum schemas",
            description="Nested elicitation/create exercising SEP-1330 enum schemas.",
            input_schema=EMPTY_OBJECT_SCHEMA, effect="read",
            handler=elicitation_sep1330_enums),
    ),
    resources=(
        Resource(uri="test://static-text", name="Static text",
            description="Official conformance text resource", mime_type="text/plain"),
        Resource(uri="test://static-binary", name="Static binary",
            description="Official conformance binary resource", mime_type="image/png"),
    ),
    resource_templates=(ResourceTemplate(
        uri_template="test://template/{id}/data", name="Template data",
        description="Official conformance resource template", mime_type="text/plain",
    ),),
    read_resource=read_resource,
)


if __name__ == "__main__":
    raise SystemExit(serve_provider(PROVIDER))
