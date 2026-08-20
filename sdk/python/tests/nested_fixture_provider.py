#!/usr/bin/env python3
"""Provider fixture that opens a real nested client request mid-call.

Spawned by the real host binary, which scrubs the environment before exec, so
the SDK is put on sys.path here rather than through PYTHONPATH - the same thing
a provider shipped as a standalone script has to do.
"""

from pathlib import Path
import sys


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from maelys_mcp_provider import (  # noqa: E402 - after the sys.path line above
    NestedRequestError, Tool, complete_result, create_provider, serve_provider,
)


def ask(arguments, context):
    """Asks the client, and reports the refusal as text when refused.

    The refusal path is text rather than an exception on purpose: the harness
    checks the capability gate by reading the answer, and a provider error
    would say only that something went wrong.
    """
    try:
        answer = context.request_elicitation({
            "message": arguments.get("message", "Proceed?"),
            "requestedSchema": {
                "type": "object",
                "properties": {"answer": {"type": "string"}},
                "required": ["answer"],
            },
        })
    except NestedRequestError as error:
        return complete_result(content=[
            {"type": "text", "text": f"error:{error.code}"}])
    return complete_result(content=[
        {"type": "text", "text": str(answer.get("answer"))}])


provider = create_provider("python-nested", "1.0.0", [Tool(
    name="nested.ask",
    description="Opens a nested client request and returns what it answered.",
    input_schema={
        "type": "object",
        "properties": {"message": {"type": "string"}},
        "additionalProperties": False,
    },
    effect="read",
    handler=ask,
)])

raise SystemExit(serve_provider(provider))
