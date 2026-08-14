"""Dependency-free SDK for persistent maelys-provider/2 processes."""
from __future__ import annotations

import json
import os
import sys
from dataclasses import dataclass
from typing import Any, Callable, Iterable, TextIO

PROTOCOL = "maelys-provider/2"
TOOL_EFFECTS = ("read", "preview", "apply", "commit", "execute")
SUPPORTED_SCHEMA_KEYS = {
    "$schema", "title", "description", "type", "properties", "required",
    "additionalProperties", "items", "enum", "minLength", "maxLength",
    "minimum", "maximum",
}
SCHEMA_TYPES = {"object", "array", "string", "number", "integer", "boolean", "null"}

JsonObject = dict[str, Any]
_UNSET = object()
@dataclass(frozen=True)
class CallContext:
    input_responses: JsonObject | None = None
    request_state: str | None = None
    client_capabilities: JsonObject | None = None


@dataclass(frozen=True)
class ProviderResult:
    result_type: str
    content: list[JsonObject] | None = None
    structured_content: Any = _UNSET
    input_requests: JsonObject | None = None
    request_state: str | None = None
    is_error: bool = False

    def payload(self) -> JsonObject:
        if self.result_type == "complete":
            if self.content is None and self.structured_content is _UNSET:
                raise TypeError("complete result requires content or structured_content")
            result: JsonObject = {"resultType": "complete"}
            if self.content is not None:
                if not isinstance(self.content, list) or not self.content:
                    raise TypeError("complete result content must be a non-empty list")
                result["content"] = self.content
            if self.structured_content is not _UNSET:
                result["structuredContent"] = self.structured_content
            if self.is_error:
                result["isError"] = True
            return result
        if self.result_type == "input_required":
            if self.input_requests is None and self.request_state is None:
                raise TypeError("input_required result requires input_requests or request_state")
            result = {"resultType": "input_required"}
            if self.input_requests is not None:
                requests = _object(self.input_requests, "input_requests")
                if not requests:
                    raise TypeError("input_requests must not be empty")
                result["inputRequests"] = requests
            if self.request_state is not None:
                result["requestState"] = _string(self.request_state, "request_state")
            return result
        raise TypeError("provider result_type must be complete or input_required")


def complete_result(
    *, content: list[JsonObject] | None = None,
    structured_content: Any = _UNSET,
    is_error: bool = False,
) -> ProviderResult:
    return ProviderResult("complete", content=content,
        structured_content=structured_content, is_error=is_error)


def input_required_result(
    *, input_requests: JsonObject | None = None,
    request_state: str | None = None,
) -> ProviderResult:
    return ProviderResult("input_required", input_requests=input_requests,
        request_state=request_state)


ToolHandler = Callable[[JsonObject, CallContext], ProviderResult]


class DuplicateKeyError(ValueError):
    pass


def _strict_object(pairs: list[tuple[str, Any]]) -> JsonObject:
    result: JsonObject = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateKeyError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _object(value: Any, label: str) -> JsonObject:
    if not isinstance(value, dict):
        raise TypeError(f"{label} must be an object")
    return value


def _string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise TypeError(f"{label} must be a non-empty string")
    return value


def _matches_type(schema_type: str, value: Any) -> bool:
    if schema_type == "object":
        return isinstance(value, dict)
    if schema_type == "array":
        return isinstance(value, list)
    if schema_type == "string":
        return isinstance(value, str)
    if schema_type == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if schema_type == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if schema_type == "boolean":
        return isinstance(value, bool)
    return value is None


def validate_schema_definition(schema: Any, label: str = "schema") -> JsonObject:
    value = _object(schema, label)
    unknown = set(value) - SUPPORTED_SCHEMA_KEYS
    if unknown:
        raise TypeError(f"{label} uses unsupported keys: {', '.join(sorted(unknown))}")
    schema_type = value.get("type")
    if schema_type not in SCHEMA_TYPES:
        raise TypeError(f"{label}.type is missing or unsupported")
    if any(keyword in value for keyword in ("properties", "required", "additionalProperties")) and schema_type != "object":
        raise TypeError(f"{label}: object keywords require type object")
    if "properties" in value:
        if schema_type != "object":
            raise TypeError(f"{label}.properties requires an object schema")
        for name, child in _object(value["properties"], f"{label}.properties").items():
            validate_schema_definition(child, f"{label}.properties.{name}")
    if "required" in value:
        required = value["required"]
        if schema_type != "object" or not isinstance(required, list) or not all(isinstance(item, str) for item in required):
            raise TypeError(f"{label}.required must be a string array on an object schema")
        if len(required) != len(set(required)):
            raise TypeError(f"{label}.required contains duplicates")
        properties = value.get("properties", {})
        if any(item not in properties for item in required):
            raise TypeError(f"{label}.required references an undeclared property")
    if "additionalProperties" in value and not isinstance(value["additionalProperties"], bool):
        raise TypeError(f"{label}.additionalProperties must be boolean")
    if "items" in value:
        if schema_type != "array":
            raise TypeError(f"{label}.items requires an array schema")
        validate_schema_definition(value["items"], f"{label}.items")
    if "enum" in value and (not isinstance(value["enum"], list) or not value["enum"]):
        raise TypeError(f"{label}.enum must be a non-empty array")
    if isinstance(value.get("enum"), list) and any(not _matches_type(schema_type, candidate) for candidate in value["enum"]):
        raise TypeError(f"{label}.enum contains a value that does not match type {schema_type}")
    if any(keyword in value for keyword in ("minLength", "maxLength")) and schema_type != "string":
        raise TypeError(f"{label}: length keywords require type string")
    for keyword in ("minLength", "maxLength"):
        bound = value.get(keyword)
        if bound is not None and (isinstance(bound, bool) or not isinstance(bound, int) or bound < 0):
            raise TypeError(f"{label}.{keyword} must be a non-negative integer")
    if value.get("minLength", 0) > value.get("maxLength", float("inf")):
        raise TypeError(f"{label} has inconsistent string bounds")
    for keyword in ("minimum", "maximum"):
        bound = value.get(keyword)
        if bound is not None and (isinstance(bound, bool) or not isinstance(bound, (int, float))):
            raise TypeError(f"{label}.{keyword} must be a number")
    if any(keyword in value for keyword in ("minimum", "maximum")) and schema_type not in {"number", "integer"}:
        raise TypeError(f"{label}: numeric bounds require type number or integer")
    if value.get("minimum", float("-inf")) > value.get("maximum", float("inf")):
        raise TypeError(f"{label} has inconsistent numeric bounds")
    return value


@dataclass(frozen=True)
class Tool:
    name: str
    description: str
    input_schema: JsonObject
    effect: str
    handler: ToolHandler
    output_schema: JsonObject | None = None
    title: str | None = None

    def descriptor(self) -> JsonObject:
        result: JsonObject = {
            "name": self.name,
            "description": self.description,
            "inputSchema": self.input_schema,
            "effect": self.effect,
        }
        if self.output_schema is not None:
            result["outputSchema"] = self.output_schema
        if self.title is not None:
            result["title"] = self.title
        return result


@dataclass(frozen=True)
class Provider:
    name: str
    version: str
    tools: tuple[Tool, ...]

    def description(self) -> JsonObject:
        return {"name": self.name, "version": self.version, "tools": [tool.descriptor() for tool in self.tools]}


def create_provider(name: str, version: str, tools: Iterable[Tool]) -> Provider:
    _string(name, "provider.name")
    _string(version, "provider.version")
    prepared = tuple(tools)
    names: set[str] = set()
    for index, tool in enumerate(prepared):
        if not isinstance(tool, Tool):
            raise TypeError(f"tools[{index}] must be a Tool")
        _string(tool.name, f"tools[{index}].name")
        _string(tool.description, f"tools[{index}].description")
        if tool.name in names:
            raise TypeError(f"duplicate provider tool: {tool.name}")
        names.add(tool.name)
        if tool.effect not in TOOL_EFFECTS:
            raise TypeError(f"tools[{index}].effect is unsupported")
        if not callable(tool.handler):
            raise TypeError(f"tools[{index}].handler must be callable")
        validate_schema_definition(tool.input_schema, f"tools[{index}].inputSchema")
        if tool.input_schema.get("type") != "object":
            raise TypeError(f"tools[{index}].inputSchema.type must be object")
        if tool.output_schema is not None:
            validate_schema_definition(tool.output_schema, f"tools[{index}].outputSchema")
    return Provider(name=name, version=version, tools=prepared)


def handle_message(provider: Provider, message: Any) -> JsonObject:
    request = _object(message, "provider request")
    request_id = request.get("id")
    if isinstance(request_id, bool) or not isinstance(request_id, int):
        raise TypeError("provider request id must be an integer")
    if request.get("protocol") != PROTOCOL:
        raise TypeError("unsupported provider protocol")
    params = _object(request.get("params", {}), "provider request params")
    method = request.get("method")
    if method == "provider/describe":
        result: Any = provider.description()
    elif method == "provider/call":
        name = _string(params.get("name"), "provider/call name")
        arguments = _object(params.get("arguments", {}), "provider/call arguments")
        tool = next((candidate for candidate in provider.tools if candidate.name == name), None)
        if tool is None:
            raise ValueError(f"unknown provider tool: {name}")
        input_responses = params.get("inputResponses")
        request_state = params.get("requestState")
        context = CallContext(
            input_responses=None if input_responses is None else
                _object(input_responses, "provider/call inputResponses"),
            request_state=None if request_state is None else
                _string(request_state, "provider/call requestState"),
            client_capabilities=None if params.get("clientCapabilities") is None else
                _object(params["clientCapabilities"], "provider/call clientCapabilities"),
        )
        provider_result = tool.handler(arguments, context)
        if not isinstance(provider_result, ProviderResult):
            raise TypeError("tool handler must return ProviderResult")
        result = provider_result.payload()
    elif method == "provider/shutdown":
        result = {}
    else:
        raise ValueError(f"unknown provider method: {method}")
    return {"protocol": PROTOCOL, "id": request_id, "result": result}


def _isolated_transport() -> TextIO:
    transport_fd = os.dup(sys.stdout.fileno())
    os.set_inheritable(transport_fd, False)
    os.dup2(sys.stderr.fileno(), sys.stdout.fileno())
    return os.fdopen(transport_fd, "w", encoding="utf-8", buffering=1)


def serve_provider(
    provider: Provider,
    input_stream: TextIO | None = None,
    transport_stream: TextIO | None = None,
    isolate_stdout: bool = True,
) -> int:
    source = input_stream or sys.stdin
    owns_transport = transport_stream is None and isolate_stdout
    transport = transport_stream or (_isolated_transport() if isolate_stdout else sys.stdout)
    try:
        for line in source:
            message: Any = None
            try:
                message = json.loads(line, object_pairs_hook=_strict_object)
                response = handle_message(provider, message)
            except Exception as error:
                candidate = message.get("id") if isinstance(message, dict) else 0
                request_id = candidate if isinstance(candidate, int) and not isinstance(candidate, bool) else 0
                response = {"protocol": PROTOCOL, "id": request_id, "error": {"message": str(error) or type(error).__name__}}
            transport.write(json.dumps(response, ensure_ascii=False, separators=(",", ":")) + "\n")
            transport.flush()
            if isinstance(message, dict) and message.get("method") == "provider/shutdown":
                break
    finally:
        if owns_transport:
            transport.close()
    return 0


__all__ = [
    "PROTOCOL", "TOOL_EFFECTS", "CallContext", "Provider", "ProviderResult",
    "Tool", "complete_result", "create_provider", "handle_message",
    "input_required_result", "serve_provider", "validate_schema_definition",
]
