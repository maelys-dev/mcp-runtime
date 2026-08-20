"""Dependency-free SDK for persistent maelys-provider/5 processes."""
from __future__ import annotations

import json
import os
import sys
import threading
from collections import deque
from dataclasses import dataclass, field
from typing import Any, Callable, Iterable, TextIO

PROTOCOL = "maelys-provider/5"
# The version every provider speaks, and the one the host opens with until
# this SDK declares a newer one in a frame of its own.
PROTOCOL_FLOOR = "maelys-provider/3"
# Every version between the floor and the current one, oldest first. The list
# matters as much as its endpoints: the host addresses a provider at whatever
# that provider last declared, and this SDK declares /5 only once it actually
# nests (see PROTOCOL_DECLARED), so /4 is a version it must expect to be
# spoken to at - a two-value "floor or newest" check would reject it.
PROTOCOL_VERSIONS = (PROTOCOL_FLOOR, "maelys-provider/4", PROTOCOL)
# The version stamped on outbound frames until a nested request is opened.
# Announcing /5 up front would cost every provider built on this SDK its
# compatibility with a host that predates /5, in exchange for a feature it may
# never use. The host learns a version from any frame rather than only from
# responses, precisely so that /5 can be announced late - on the frame that
# opens the first nested request - which is what keeps a provider that never
# nests emitting exactly the bytes it emitted before this version existed.
PROTOCOL_DECLARED = "maelys-provider/4"
TOOL_EFFECTS = ("read", "preview", "apply", "commit", "execute")
SUPPORTED_SCHEMA_KEYS = {
    "$schema", "title", "description", "type", "properties", "required",
    "additionalProperties", "items", "enum", "minLength", "maxLength",
    "minimum", "maximum",
}
SCHEMA_TYPES = {"object", "array", "string", "number", "integer", "boolean", "null"}

JsonObject = dict[str, Any]
_UNSET = object()


class NestedRequestError(RuntimeError):
    """A nested client request that came back as anything but a result.

    `code` is the host's own code - `client_error`, `denied`, `timeout`,
    `cancelled`, `unavailable` or `failed` - or one this SDK raises on its own
    behalf when the wire, rather than the client, is what went wrong. `data`
    carries the client's JSON-RPC error object for `client_error`, so a handler
    can tell "the user declined" from "the request never got there".
    """

    def __init__(self, code: str, message: str, data: Any = None) -> None:
        super().__init__(message)
        self.code = code
        self.data = data


class NestedRequestDenied(NestedRequestError):
    """The host refused to forward the request; no byte reached the client."""


class NestedRequestTimeout(NestedRequestError):
    """The client did not answer within the host's nested deadline."""


class NestedRequestCancelled(NestedRequestError):
    """The outer call was cancelled, or the client connection went away."""


class NestedRequestUnavailable(NestedRequestError):
    """This call cannot nest: no transport, or no call in progress."""


class NestedTransportError(NestedRequestError):
    """The provider wire failed rather than the client: closed, or malformed.

    Deliberately a NestedRequestError as well, so one except clause covers
    every way a helper below can fail to produce an answer.
    """


_NESTED_ERRORS: dict[str, type[NestedRequestError]] = {
    "denied": NestedRequestDenied,
    "timeout": NestedRequestTimeout,
    "cancelled": NestedRequestCancelled,
    "unavailable": NestedRequestUnavailable,
}


@dataclass(frozen=True)
class CallContext:
    input_responses: JsonObject | None = None
    request_state: str | None = None
    client_capabilities: JsonObject | None = None
    # Bound by serve_provider for the duration of one handler call. Excluded
    # from equality and repr so a context still compares by what it carries.
    nested: Any = field(default=None, compare=False, repr=False)

    def request_elicitation(self, params: JsonObject | None = None) -> JsonObject:
        """Asks the client a question and blocks for the answer.

        `params` is the MCP `elicitation/create` params object, passed through
        verbatim - the host forwards it and this SDK does not second-guess the
        client's schema, exactly as `input_required_result` does not.

        The counterpart of `input_required_result`, not a replacement for it:
        that one ends the call and lets the client retry, this one keeps the
        call open. Raises NestedRequestError (see its subclasses) when the
        answer is a refusal rather than a result, and must be called from the
        thread handling the call.
        """
        return self._request("elicitation/create", params)

    def request_sampling(self, params: JsonObject | None = None) -> JsonObject:
        """Asks the client's model for a completion and blocks for it."""
        return self._request("sampling/createMessage", params)

    def request_roots(self, params: JsonObject | None = None) -> JsonObject:
        """Asks the client for its roots and blocks for them."""
        return self._request("roots/list", params)

    def _request(self, method: str, params: JsonObject | None) -> JsonObject:
        if self.nested is None:
            raise NestedRequestUnavailable("unavailable",
                "nested requests need a serve_provider transport")
        return self.nested.request(method,
            {} if params is None else _object(params, f"{method} params"))


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


@dataclass(frozen=True)
class ResourceResult:
    result_type: str
    contents: list[JsonObject] | None = None
    input_requests: JsonObject | None = None
    request_state: str | None = None

    def payload(self) -> JsonObject:
        if self.result_type == "complete":
            if not isinstance(self.contents, list) or not self.contents:
                raise TypeError("resource result contents must be a non-empty list")
            return {"resultType": "complete", "contents": self.contents}
        if self.result_type == "input_required":
            return ProviderResult("input_required", input_requests=self.input_requests,
                request_state=self.request_state).payload()
        raise TypeError("resource result_type must be complete or input_required")


def resource_result(contents: list[JsonObject]) -> ResourceResult:
    return ResourceResult("complete", contents=contents)


def resource_input_required_result(
    *, input_requests: JsonObject | None = None,
    request_state: str | None = None,
) -> ResourceResult:
    return ResourceResult("input_required", input_requests=input_requests,
        request_state=request_state)


ToolHandler = Callable[[JsonObject, CallContext], ProviderResult]
ResourceHandler = Callable[[str, CallContext], ResourceResult]


class ProviderEvents:
    """Thread-safe provider/3 event facade, active only after host activation."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._writer: Callable[[JsonObject], None] | None = None
        self._active = False

    def _bind(self, writer: Callable[[JsonObject], None]) -> None:
        with self._lock:
            self._writer = writer

    def _activate(self) -> None:
        with self._lock:
            self._active = True

    def _deactivate(self) -> None:
        with self._lock:
            self._active = False
            self._writer = None

    def _emit(self, method: str, params: JsonObject | None = None) -> None:
        with self._lock:
            if not self._active or self._writer is None:
                raise RuntimeError(
                    "provider events are unavailable before activation or after shutdown")
            # The writer stamps the version, rather than every caller repeating
            # it: what this provider declares can change mid-session, and an
            # event emitted from another thread must declare what the session
            # declares now, not what it declared when this method was written.
            self._writer({"method": method, "params": params or {}})

    def report_progress(
        self,
        progress: float,
        total: float | None = None,
        message: str | None = None,
    ) -> None:
        """Reports progress for the call being handled.

        Request-scoped, unlike the events below: the host routes it to the one
        call in flight, so it must only be sent from inside a tool handler. No
        progress token is carried - the host holds it, which is what stops a
        provider addressing progress at a request that is not its own.

        Best effort: a host whose client asked for no progress simply drops
        it, so a handler may report unconditionally.
        """
        params: JsonObject = {"progress": progress}
        if total is not None:
            params["total"] = total
        if message is not None:
            params["message"] = message
        self._emit("provider/notifications/progress", params)

    def resource_updated(self, uri: str) -> None:
        self._emit("provider/notifications/resources/updated",
            {"uri": _string(uri, "resource event uri")})

    def resources_list_changed(self) -> None:
        self._emit("provider/notifications/resources/list_changed")

    def tools_list_changed(self) -> None:
        self._emit("provider/notifications/tools/list_changed")


class DuplicateKeyError(ValueError):
    pass


class ProviderNotFoundError(LookupError):
    """Signal that a requested provider resource does not exist."""


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
class Resource:
    uri: str
    name: str
    title: str | None = None
    description: str | None = None
    mime_type: str | None = None
    size: int | None = None

    def descriptor(self) -> JsonObject:
        result: JsonObject = {"uri": self.uri, "name": self.name}
        if self.title is not None:
            result["title"] = self.title
        if self.description is not None:
            result["description"] = self.description
        if self.mime_type is not None:
            result["mimeType"] = self.mime_type
        if self.size is not None:
            result["size"] = self.size
        return result


@dataclass(frozen=True)
class ResourceTemplate:
    uri_template: str
    name: str
    title: str | None = None
    description: str | None = None
    mime_type: str | None = None

    def descriptor(self) -> JsonObject:
        result: JsonObject = {"uriTemplate": self.uri_template, "name": self.name}
        if self.title is not None:
            result["title"] = self.title
        if self.description is not None:
            result["description"] = self.description
        if self.mime_type is not None:
            result["mimeType"] = self.mime_type
        return result


@dataclass(frozen=True)
class Provider:
    name: str
    version: str
    tools: tuple[Tool, ...]
    resources: tuple[Resource, ...] = ()
    resource_templates: tuple[ResourceTemplate, ...] = ()
    read_resource: ResourceHandler | None = None
    events: ProviderEvents = field(default_factory=ProviderEvents, compare=False)

    def description(self) -> JsonObject:
        result: JsonObject = {"name": self.name, "version": self.version,
            "tools": [tool.descriptor() for tool in self.tools]}
        if self.resources:
            result["resources"] = [resource.descriptor() for resource in self.resources]
        if self.resource_templates:
            result["resourceTemplates"] = [resource.descriptor() for resource in self.resource_templates]
        return result


def create_provider(
    name: str,
    version: str,
    tools: Iterable[Tool] = (),
    *,
    resources: Iterable[Resource] = (),
    resource_templates: Iterable[ResourceTemplate] = (),
    read_resource: ResourceHandler | None = None,
) -> Provider:
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
    prepared_resources = tuple(resources)
    prepared_templates = tuple(resource_templates)
    resource_uris: set[str] = set()
    for index, resource in enumerate(prepared_resources):
        if not isinstance(resource, Resource):
            raise TypeError(f"resources[{index}] must be a Resource")
        _string(resource.uri, f"resources[{index}].uri")
        _string(resource.name, f"resources[{index}].name")
        if resource.uri in resource_uris:
            raise TypeError(f"duplicate provider resource: {resource.uri}")
        resource_uris.add(resource.uri)
        if resource.size is not None and (isinstance(resource.size, bool) or
                not isinstance(resource.size, int) or resource.size < 0):
            raise TypeError(f"resources[{index}].size must be a non-negative integer")
    template_uris: set[str] = set()
    for index, resource in enumerate(prepared_templates):
        if not isinstance(resource, ResourceTemplate):
            raise TypeError(f"resource_templates[{index}] must be a ResourceTemplate")
        _string(resource.uri_template, f"resource_templates[{index}].uri_template")
        _string(resource.name, f"resource_templates[{index}].name")
        if resource.uri_template in template_uris:
            raise TypeError(f"duplicate provider resource template: {resource.uri_template}")
        template_uris.add(resource.uri_template)
    if (prepared_resources or prepared_templates) and not callable(read_resource):
        raise TypeError("read_resource is required when resources are declared")
    return Provider(name=name, version=version, tools=prepared,
        resources=prepared_resources, resource_templates=prepared_templates,
        read_resource=read_resource, events=ProviderEvents())


@dataclass(frozen=True)
class _Frame:
    """One line off the wire, parsed exactly once.

    A frame set aside during a nested wait is handed to the dispatch loop
    later, and a line that did not parse has to reach it as the same failure it
    would have been inline - so the parse result travels with the frame rather
    than being redone where it is used.
    """

    message: Any = None
    error: Exception | None = None


def _parse_frame(line: str) -> _Frame:
    try:
        return _Frame(message=json.loads(line, object_pairs_hook=_strict_object))
    except Exception as error:  # reported as a provider error, like inline
        return _Frame(error=error)


class _NestedCall:
    """The nested-request surface for exactly one call.

    Scoped to the call rather than to the session for the same reason
    ProviderEvents is gated on activation: the host fails the whole provider
    transport for a nested request arriving while no call is being handled, so
    a context a handler stashed and used afterwards has to fail here, on this
    side of the wire, instead of killing the process.
    """

    def __init__(self, session: "_Session") -> None:
        self._session = session

    def request(self, method: str, params: JsonObject) -> JsonObject:
        return self._session.request(self, method, params)


class _Session:
    """What one serve_provider loop shares with the handler inside a call.

    Three things live here because both the outer dispatch and an in-call
    nested wait need them and must not disagree: the single point where lines
    are taken off the wire, the single point where frames are written, and the
    version declared on them.

    The intake is the reason this class exists. A handler blocked on a nested
    reply has to keep reading, because the reply arrives on the same stream the
    dispatch loop reads - but anything else that arrives meanwhile is not the
    handler's to answer. Those frames are set aside in arrival order and handed
    to the dispatch loop when the call returns, so an interleaved frame loses
    its turn but never its place, and nothing is read twice or dropped.
    """

    def __init__(
        self,
        source: TextIO,
        write_message: Callable[[JsonObject], None],
    ) -> None:
        self._source = source
        self._write_message = write_message
        self._lock = threading.Lock()
        self._deferred: deque[_Frame] = deque()
        self._protocol = PROTOCOL_DECLARED
        self._counter = 0
        self._call: _NestedCall | None = None
        self._outstanding = False

    @property
    def protocol(self) -> str:
        with self._lock:
            return self._protocol

    def write(self, message: JsonObject) -> None:
        """Stamps the declared version and writes one frame.

        Stamped here rather than by each caller because the declared version
        can change mid-session; first, so the encoded key order is unchanged.
        """
        self._write_message({"protocol": self.protocol, **message})

    def next_frame(self) -> _Frame | None:
        """The next frame for the dispatch loop, or None at end of input.

        Reading, and the deferred queue behind it, belong to the thread running
        the dispatch loop - which is the same thread a handler blocks on, so
        the two never race for a line.
        """
        if self._deferred:
            return self._deferred.popleft()
        line = self._source.readline()
        return _parse_frame(line) if line else None

    def open_call(self, call: _NestedCall) -> None:
        with self._lock:
            self._call = call

    def close_call(self, call: _NestedCall) -> None:
        with self._lock:
            if self._call is call:
                self._call = None

    def request(
        self,
        call: _NestedCall,
        method: str,
        params: JsonObject,
    ) -> JsonObject:
        with self._lock:
            if self._call is not call:
                raise NestedRequestUnavailable("unavailable",
                    "a nested request is only valid inside the call it belongs to")
            if self._outstanding:
                # The host fails the transport over two of these at once, so
                # refusing the second here costs a handler an exception instead
                # of costing the provider its connection.
                raise NestedRequestUnavailable("unavailable",
                    "this call already has a nested request outstanding")
            self._outstanding = True
            self._counter += 1
            nested_id = f"n{self._counter}"
            # Raised before the frame goes out, never lowered afterwards: the
            # host learns a provider's version from any frame, and this is the
            # frame that announces /5.
            self._protocol = PROTOCOL
        try:
            self.write({"method": "provider/nestedRequest", "params": {
                "nestedId": nested_id, "method": method, "params": params}})
            return _nested_result(self._await_reply(nested_id))
        finally:
            with self._lock:
                self._outstanding = False

    def _await_reply(self, nested_id: str) -> JsonObject:
        while True:
            line = self._source.readline()
            if not line:
                raise NestedTransportError("closed",
                    "the transport closed before the nested request was answered")
            frame = _parse_frame(line)
            params = _nested_reply_params(frame, nested_id)
            if params is None:
                self._deferred.append(frame)
                continue
            return params


def _nested_reply_params(frame: _Frame, nested_id: str) -> JsonObject | None:
    """The params of the host's reply to `nested_id`, or None when this frame
    is something else the dispatch loop still has to see."""
    message = frame.message
    if frame.error is not None or not isinstance(message, dict):
        return None
    if message.get("method") != "provider/nestedReply":
        return None
    if message.get("protocol") not in PROTOCOL_VERSIONS:
        raise NestedTransportError("malformed",
            "nested reply declares an unsupported provider protocol")
    params = message.get("params")
    if not isinstance(params, dict):
        raise NestedTransportError("malformed",
            "nested reply params must be an object")
    if params.get("nestedId") != nested_id:
        # Not deferred: nestedId is echoed verbatim and only one request is
        # ever outstanding, so a reply addressed at anything else means the two
        # ends disagree about what is being answered.
        raise NestedTransportError("malformed",
            "nested reply does not correlate with the request")
    return params


def _nested_result(params: JsonObject) -> JsonObject:
    result = params.get("result")
    error = params.get("error")
    if (result is None) == (error is None):
        raise NestedTransportError("malformed",
            "nested reply must carry exactly one result or error")
    if error is not None:
        if not isinstance(error, dict):
            raise NestedTransportError("malformed",
                "nested reply error must be an object")
        code = error.get("code")
        code = code if isinstance(code, str) and code else "failed"
        text = error.get("message")
        raise _NESTED_ERRORS.get(code, NestedRequestError)(code,
            text if isinstance(text, str) and text else "the nested request failed",
            error.get("data"))
    if not isinstance(result, dict):
        raise NestedTransportError("malformed",
            "nested reply result must be an object")
    return result


def _call_context(params: JsonObject, label: str, nested: _NestedCall | None) -> CallContext:
    input_responses = params.get("inputResponses")
    request_state = params.get("requestState")
    return CallContext(
        input_responses=None if input_responses is None else
            _object(input_responses, f"{label} inputResponses"),
        request_state=None if request_state is None else
            _string(request_state, f"{label} requestState"),
        client_capabilities=None if params.get("clientCapabilities") is None else
            _object(params["clientCapabilities"], f"{label} clientCapabilities"),
        nested=nested,
    )


def _handle_call(
    session: "_Session | None",
    nested: _NestedCall | None,
    handler: Callable[..., Any],
    *arguments: Any,
) -> Any:
    """Runs one handler with its nested window open for exactly its duration."""
    if session is None or nested is None:
        return handler(*arguments)
    session.open_call(nested)
    try:
        return handler(*arguments)
    finally:
        session.close_call(nested)


def handle_message(
    provider: Provider,
    message: Any,
    session: "_Session | None" = None,
) -> JsonObject:
    request = _object(message, "provider request")
    request_id = request.get("id")
    if isinstance(request_id, bool) or not isinstance(request_id, int):
        raise TypeError("provider request id must be an integer")
    # The host opens at the floor and raises to whatever this provider last
    # declared, which is not necessarily the newest version the SDK speaks -
    # /5 is announced late, on the first nested request - so every version in
    # the range is valid inbound, not just the two endpoints.
    if request.get("protocol") not in PROTOCOL_VERSIONS:
        raise TypeError("unsupported provider protocol")
    params = _object(request.get("params", {}), "provider request params")
    method = request.get("method")
    nested = _NestedCall(session) if session is not None else None
    if method == "provider/describe":
        result: Any = provider.description()
    elif method == "provider/activate":
        result = {}
    elif method == "provider/call":
        name = _string(params.get("name"), "provider/call name")
        arguments = _object(params.get("arguments", {}), "provider/call arguments")
        tool = next((candidate for candidate in provider.tools if candidate.name == name), None)
        if tool is None:
            raise ValueError(f"unknown provider tool: {name}")
        context = _call_context(params, "provider/call", nested)
        provider_result = _handle_call(session, nested, tool.handler, arguments, context)
        if not isinstance(provider_result, ProviderResult):
            raise TypeError("tool handler must return ProviderResult")
        result = provider_result.payload()
    elif method == "provider/readResource":
        if provider.read_resource is None:
            raise ValueError("provider does not expose resources")
        uri = _string(params.get("uri"), "provider/readResource uri")
        context = _call_context(params, "provider/readResource", nested)
        resource_result = _handle_call(session, nested, provider.read_resource, uri, context)
        if not isinstance(resource_result, ResourceResult):
            raise TypeError("resource handler must return ResourceResult")
        result = resource_result.payload()
    elif method == "provider/shutdown":
        result = {}
    else:
        raise ValueError(f"unknown provider method: {method}")
    # Read after the handler rather than before it: a call that opened a nested
    # request has already declared /5 by now, and its own response must not go
    # back out claiming the older version.
    return {"protocol": session.protocol if session is not None else PROTOCOL_DECLARED,
        "id": request_id, "result": result}


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
    write_lock = threading.Lock()

    def write_message(message: JsonObject) -> None:
        encoded = json.dumps(message, ensure_ascii=False, separators=(",", ":")) + "\n"
        with write_lock:
            transport.write(encoded)
            transport.flush()

    session = _Session(source, write_message)
    provider.events._bind(session.write)
    try:
        while True:
            frame = session.next_frame()
            if frame is None:
                break
            message: Any = frame.message
            try:
                if frame.error is not None:
                    raise frame.error
                response = handle_message(provider, message, session)
            except Exception as error:
                candidate = message.get("id") if isinstance(message, dict) else 0
                request_id = candidate if isinstance(candidate, int) and not isinstance(candidate, bool) else 0
                code = "not_found" if isinstance(error, ProviderNotFoundError) else "provider_error"
                response = {"protocol": session.protocol, "id": request_id,
                    "error": {"code": code, "message": str(error) or type(error).__name__}}
            write_message(response)
            if (isinstance(message, dict) and
                    message.get("method") == "provider/activate" and
                    "result" in response):
                provider.events._activate()
            if isinstance(message, dict) and message.get("method") == "provider/shutdown":
                break
    finally:
        provider.events._deactivate()
        if owns_transport:
            transport.close()
    return 0


__all__ = [
    "PROTOCOL", "PROTOCOL_DECLARED", "PROTOCOL_FLOOR", "PROTOCOL_VERSIONS",
    "TOOL_EFFECTS", "CallContext", "NestedRequestCancelled", "NestedRequestDenied",
    "NestedRequestError", "NestedRequestTimeout", "NestedRequestUnavailable",
    "NestedTransportError", "Provider", "ProviderEvents", "ProviderNotFoundError", "ProviderResult", "ResourceResult",
    "Resource", "ResourceTemplate", "Tool", "complete_result", "create_provider", "handle_message",
    "input_required_result", "resource_input_required_result", "resource_result", "serve_provider",
    "validate_schema_definition",
]
