#!/usr/bin/env python3
"""Black-box conformance runner for persistent maelys-provider executables.

Speaks the wire directly to one provider subprocess, playing the host's side
of the protocol - including, for a case that declares a `nested` exchange,
the host's side of the `provider/nestedRequest` / `provider/nestedReply`
round trip (docs/provider-protocol.md "Nested requests"). That is why this
runner is interactive (one request written, its response read, before the
next is written) rather than batching every request up front the way it did
before nested support existed: a provider blocked mid-call waiting on a
nested reply would never see the rest of a batch that was already flushed to
its stdin, and a batch-then-read-everything runner has nothing to answer it
with until long after the provider gave up.
"""
from __future__ import annotations

import argparse
import json
import os
import queue
import re
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any

# The runner opens at the floor, like the host does, and accepts either
# version back: a provider declares its own in responses rather than echoing
# ours, which is how the host learns whether it may send progress.
PROTOCOL = "maelys-provider/3"
SUPPORTED_PROTOCOLS = (
    "maelys-provider/3",
    "maelys-provider/4",
    "maelys-provider/5",
)
EFFECTS = {"read", "preview", "apply", "commit", "execute"}
SUPPORTED_SCHEMA_KEYS = {
    "$schema", "title", "description", "type", "properties", "required",
    "additionalProperties", "items", "enum", "minLength", "maxLength",
    "minimum", "maximum",
}


def matches_type(schema_type: str, value: Any) -> bool:
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


class ConformanceError(RuntimeError):
    pass


class DuplicateKeyError(ValueError):
    pass


def strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateKeyError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_json(text: str, label: str) -> Any:
    try:
        return json.loads(text, object_pairs_hook=strict_object)
    except (json.JSONDecodeError, DuplicateKeyError) as error:
        raise ConformanceError(f"{label} is not strict JSON: {error}") from error


def validate_schema(schema: Any, label: str) -> None:
    if not isinstance(schema, dict):
        raise ConformanceError(f"{label} must be an object")
    unknown = set(schema) - SUPPORTED_SCHEMA_KEYS
    if unknown:
        raise ConformanceError(f"{label} uses unsupported keys: {', '.join(sorted(unknown))}")
    schema_type = schema.get("type")
    if schema_type not in {"object", "array", "string", "number", "integer", "boolean", "null"}:
        raise ConformanceError(f"{label}.type is missing or unsupported")
    if any(keyword in schema for keyword in ("properties", "required", "additionalProperties")) and schema_type != "object":
        raise ConformanceError(f"{label}: object keywords require type object")
    if "properties" in schema:
        if schema_type != "object" or not isinstance(schema["properties"], dict):
            raise ConformanceError(f"{label}.properties requires an object schema")
        for name, child in schema["properties"].items():
            validate_schema(child, f"{label}.properties.{name}")
    if "required" in schema:
        required = schema["required"]
        if schema_type != "object" or not isinstance(required, list) or not all(isinstance(item, str) for item in required):
            raise ConformanceError(f"{label}.required must be a string array on an object schema")
        if len(required) != len(set(required)):
            raise ConformanceError(f"{label}.required contains duplicates")
        declared = schema.get("properties", {})
        if any(item not in declared for item in required):
            raise ConformanceError(f"{label}.required references an undeclared property")
    if "additionalProperties" in schema and not isinstance(schema["additionalProperties"], bool):
        raise ConformanceError(f"{label}.additionalProperties must be boolean")
    if "items" in schema:
        if schema_type != "array":
            raise ConformanceError(f"{label}.items requires an array schema")
        validate_schema(schema["items"], f"{label}.items")
    if "enum" in schema and (not isinstance(schema["enum"], list) or not schema["enum"]):
        raise ConformanceError(f"{label}.enum must be a non-empty array")
    if isinstance(schema.get("enum"), list) and any(not matches_type(schema_type, candidate) for candidate in schema["enum"]):
        raise ConformanceError(f"{label}.enum contains a value that does not match type {schema_type}")
    if any(keyword in schema for keyword in ("minLength", "maxLength")) and schema_type != "string":
        raise ConformanceError(f"{label}: length keywords require type string")
    for keyword in ("minLength", "maxLength"):
        bound = schema.get(keyword)
        if bound is not None and (isinstance(bound, bool) or not isinstance(bound, int) or bound < 0):
            raise ConformanceError(f"{label}.{keyword} must be a non-negative integer")
    if schema.get("minLength", 0) > schema.get("maxLength", float("inf")):
        raise ConformanceError(f"{label} has inconsistent string bounds")
    if any(keyword in schema for keyword in ("minimum", "maximum")) and schema_type not in {"number", "integer"}:
        raise ConformanceError(f"{label}: numeric bounds require type number or integer")
    for keyword in ("minimum", "maximum"):
        bound = schema.get(keyword)
        if bound is not None and (isinstance(bound, bool) or not isinstance(bound, (int, float))):
            raise ConformanceError(f"{label}.{keyword} must be a number")
    if schema.get("minimum", float("-inf")) > schema.get("maximum", float("inf")):
        raise ConformanceError(f"{label} has inconsistent numeric bounds")


def request(request_id: int, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
    return {"protocol": PROTOCOL, "id": request_id, "method": method, "params": params or {}}


def validate_envelope(response: Any, request_id: int, expect_error: bool) -> Any:
    if not isinstance(response, dict):
        raise ConformanceError(f"response {request_id} must be an object")
    if response.get("protocol") not in SUPPORTED_PROTOCOLS or response.get("id") != request_id:
        raise ConformanceError(f"response {request_id} does not preserve protocol and id")
    has_result = "result" in response
    has_error = "error" in response
    if has_result == has_error:
        raise ConformanceError(f"response {request_id} must contain exactly one of result or error")
    if expect_error:
        if not has_error or not isinstance(response["error"], dict) or not isinstance(response["error"].get("message"), str):
            raise ConformanceError(f"response {request_id} must contain a structured error")
        return response["error"]
    if has_error:
        raise ConformanceError(f"response {request_id} failed: {response['error']}")
    return response["result"]


def validate_event(message: Any) -> None:
    if (not isinstance(message, dict) or
            message.get("protocol") not in SUPPORTED_PROTOCOLS or "id" in message):
        raise ConformanceError("provider event must be an id-less provider envelope")
    method = message.get("method")
    params = message.get("params", {})
    if not isinstance(params, dict):
        raise ConformanceError("provider event params must be an object")
    if method == "provider/notifications/resources/updated":
        if not isinstance(params.get("uri"), str) or not params["uri"]:
            raise ConformanceError("resource update event requires a non-empty uri")
        return
    if method in {
        "provider/notifications/resources/list_changed",
        "provider/notifications/tools/list_changed",
    } and not params:
        return
    raise ConformanceError(f"unsupported or malformed provider event: {method}")


def is_nested_request(message: Any) -> bool:
    """True for a `provider/nestedRequest` frame - the one id-less envelope
    this runner does not treat as a provider/3 event, because it demands a
    reply rather than being fire-and-forget. See exchange()."""
    return (isinstance(message, dict) and "id" not in message and
        message.get("method") == "provider/nestedRequest")


def validate_nested_request(message: Any, spec: dict[str, Any]) -> str:
    """Checks a `provider/nestedRequest` frame against the case's declared
    expectation and returns the `nestedId` to answer.

    `spec` is one entry of a case's `nested` list (see read_cases) - the
    fixture author's own description of the request they expect the provider
    to open next, so a provider that nests the wrong method, or nests when
    the case did not expect it at all, fails loudly here rather than getting
    an answer that happens to satisfy it anyway.
    """
    if message.get("protocol") not in SUPPORTED_PROTOCOLS:
        raise ConformanceError("nested request must declare a supported provider protocol")
    params = message.get("params")
    if not isinstance(params, dict):
        raise ConformanceError("nested request params must be an object")
    nested_id = params.get("nestedId")
    if not isinstance(nested_id, str) or not nested_id:
        raise ConformanceError("nested request must carry a non-empty nestedId")
    if not isinstance(params.get("method"), str) or not params["method"]:
        raise ConformanceError("nested request must carry a non-empty method")
    if not isinstance(params.get("params"), dict):
        raise ConformanceError("nested request params.params must be an object")
    expected_method = spec.get("method")
    if expected_method is not None and params["method"] != expected_method:
        raise ConformanceError(
            f"nested request method mismatch: expected {expected_method!r}, got {params['method']!r}"
        )
    return nested_id


def build_nested_reply(nested_id: str, spec: dict[str, Any]) -> dict[str, Any]:
    """The `provider/nestedReply` this runner - playing the host - sends back
    for one entry of a case's `nested` list. Exactly one of `result`/`error`
    per docs/provider-protocol.md; `read_cases` already rejected a spec
    carrying both or neither."""
    payload: dict[str, Any] = {"nestedId": nested_id}
    if "error" in spec:
        payload["error"] = spec["error"]
    else:
        payload["result"] = spec.get("result", {})
    return {"protocol": PROTOCOL, "method": "provider/nestedReply", "params": payload}


def validate_call_result(result: Any) -> None:
    if not isinstance(result, dict):
        raise ConformanceError("provider call result must be an object")
    result_type = result.get("resultType")
    if result_type == "complete":
        if "content" not in result and "structuredContent" not in result:
            raise ConformanceError("complete result requires content or structuredContent")
        if "content" in result and (not isinstance(result["content"], list) or not result["content"]):
            raise ConformanceError("complete result content must be a non-empty array")
        return
    if result_type == "input_required":
        if "inputRequests" not in result and "requestState" not in result:
            raise ConformanceError("input_required result requires inputRequests or requestState")
        return
    raise ConformanceError("provider call resultType must be complete or input_required")


def apply_call_assertions(result: Any, case: dict[str, Any]) -> None:
    """Checks a case's optional `assertContentText` / `assertStructuredContent`
    against a successful call result.

    Structural validation (validate_call_result) only proves a result is
    shaped like a complete result - not that a nested case's round trip
    actually carried the answer this runner sent in build_nested_reply()
    back out through the provider's handler. A case that declares one of
    these two keys is asking for that stronger proof.
    """
    if "assertContentText" in case:
        expected = case["assertContentText"]
        content = result.get("content") if isinstance(result, dict) else None
        first = content[0] if isinstance(content, list) and content else None
        actual = first.get("text") if isinstance(first, dict) else None
        if actual != expected:
            raise ConformanceError(
                f"call result content[0].text was {actual!r}, expected {expected!r}"
            )
    if "assertStructuredContent" in case:
        expected = case["assertStructuredContent"]
        actual = result.get("structuredContent") if isinstance(result, dict) else None
        if actual != expected:
            raise ConformanceError(
                f"call result structuredContent was {actual!r}, expected {expected!r}"
            )


def validate_description(description: Any) -> dict[str, dict[str, Any]]:
    if not isinstance(description, dict):
        raise ConformanceError("provider description must be an object")
    if not isinstance(description.get("name"), str) or not description["name"]:
        raise ConformanceError("provider name must be a non-empty string")
    if not isinstance(description.get("version"), str) or not description["version"]:
        raise ConformanceError("provider version must be a non-empty string")
    tools = description.get("tools")
    if not isinstance(tools, list):
        raise ConformanceError("provider tools must be an array")
    catalog: dict[str, dict[str, Any]] = {}
    for index, tool in enumerate(tools):
        label = f"tools[{index}]"
        if not isinstance(tool, dict):
            raise ConformanceError(f"{label} must be an object")
        name = tool.get("name")
        if not isinstance(name, str) or not name or name in catalog:
            raise ConformanceError(f"{label}.name must be non-empty and unique")
        if not isinstance(tool.get("description"), str) or not tool["description"]:
            raise ConformanceError(f"{label}.description must be non-empty")
        if tool.get("effect") not in EFFECTS:
            raise ConformanceError(f"{label}.effect is unsupported")
        validate_schema(tool.get("inputSchema"), f"{label}.inputSchema")
        if tool["inputSchema"].get("type") != "object":
            raise ConformanceError(f"{label}.inputSchema.type must be object")
        if "outputSchema" in tool:
            validate_schema(tool["outputSchema"], f"{label}.outputSchema")
        catalog[name] = tool
    return catalog


def read_cases(path: Path | None) -> list[dict[str, Any]]:
    if path is None:
        return []
    payload = load_json(path.read_text(encoding="utf-8"), str(path))
    if not isinstance(payload, dict) or not isinstance(payload.get("calls"), list):
        raise ConformanceError("case file must contain a calls array")
    def expand(value: Any) -> Any:
        if isinstance(value, list):
            return [expand(item) for item in value]
        if isinstance(value, dict):
            return {key: expand(item) for key, item in value.items()}
        if isinstance(value, str):
            match = re.fullmatch(r"\$\{([A-Z][A-Z0-9_]*)\}", value)
            if match:
                name = match.group(1)
                if name not in os.environ:
                    raise ConformanceError(f"case environment variable is missing: {name}")
                return os.environ[name]
        return value

    calls = expand(payload["calls"])
    for index, case in enumerate(calls):
        if not isinstance(case, dict) or not isinstance(case.get("tool"), str) or not isinstance(case.get("arguments", {}), dict):
            raise ConformanceError(f"calls[{index}] must contain tool and object arguments")
        if case.get("expect", "success") not in {"success", "error"}:
            raise ConformanceError(f"calls[{index}].expect must be success or error")
        if "nested" in case:
            nested = case["nested"]
            if not isinstance(nested, list) or not nested:
                raise ConformanceError(f"calls[{index}].nested must be a non-empty array")
            for offset, spec in enumerate(nested):
                label = f"calls[{index}].nested[{offset}]"
                if not isinstance(spec, dict):
                    raise ConformanceError(f"{label} must be an object")
                if "method" in spec and (not isinstance(spec["method"], str) or not spec["method"]):
                    raise ConformanceError(f"{label}.method must be a non-empty string")
                has_result = "result" in spec
                has_error = "error" in spec
                if has_result == has_error:
                    raise ConformanceError(f"{label} must contain exactly one of result or error")
                if has_result and not isinstance(spec["result"], dict):
                    raise ConformanceError(f"{label}.result must be an object")
                if has_error and (not isinstance(spec["error"], dict) or
                        not isinstance(spec["error"].get("code"), str) or
                        not isinstance(spec["error"].get("message"), str)):
                    raise ConformanceError(f"{label}.error must have a string code and message")
        if "assertContentText" in case and not isinstance(case["assertContentText"], str):
            raise ConformanceError(f"calls[{index}].assertContentText must be a string")
        if "assertStructuredContent" in case and not isinstance(case["assertStructuredContent"], dict):
            raise ConformanceError(f"calls[{index}].assertStructuredContent must be an object")
    return calls


class ProviderSession:
    """One provider subprocess, driven one request at a time.

    A background thread pumps stdout lines into a queue rather than this
    class calling readline() directly on the deadline path: a plain
    `select()` on a text-mode pipe can report "not ready" while a complete
    line already sits in Python's own decode buffer (left over from a
    previous larger read), which would stall a request that has already been
    answered. Pulling from a queue with `Queue.get(timeout=...)` has no such
    blind spot - the pump thread only ever puts whole lines on it.
    """

    def __init__(self, executable: Path, timeout: float) -> None:
        self._deadline = time.monotonic() + timeout
        self._process = subprocess.Popen(
            [str(executable), "--provider"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1,
        )
        self._lines: queue.Queue[str | None] = queue.Queue()
        self._stdout_pump = threading.Thread(target=self._pump_stdout, daemon=True)
        self._stdout_pump.start()
        self._stderr_chunks: list[str] = []
        self._stderr_pump = threading.Thread(target=self._pump_stderr, daemon=True)
        self._stderr_pump.start()

    def _pump_stdout(self) -> None:
        assert self._process.stdout is not None
        for line in self._process.stdout:
            self._lines.put(line)
        self._lines.put(None)  # EOF sentinel: unblocks a waiter forever, not just once.

    def _pump_stderr(self) -> None:
        assert self._process.stderr is not None
        for line in self._process.stderr:
            self._stderr_chunks.append(line)

    def send(self, message: dict[str, Any]) -> None:
        assert self._process.stdin is not None
        try:
            self._process.stdin.write(json.dumps(message, ensure_ascii=False, separators=(",", ":")) + "\n")
            self._process.stdin.flush()
        except (BrokenPipeError, OSError) as error:
            raise ConformanceError(f"provider closed stdin: {error}") from error

    def read_message(self) -> Any:
        remaining = self._deadline - time.monotonic()
        if remaining <= 0:
            raise ConformanceError(f"provider produced no output within the timeout")
        try:
            line = self._lines.get(timeout=remaining)
        except queue.Empty:
            raise ConformanceError(f"provider produced no output within the timeout")
        if line is None:
            raise ConformanceError("provider closed stdout unexpectedly")
        try:
            return load_json(line, "provider stdout")
        except ConformanceError as error:
            raise ConformanceError(f"provider stdout protocol contamination: {error}") from error

    def stderr(self) -> str:
        self._stderr_pump.join(timeout=2)
        return "".join(self._stderr_chunks)

    def close(self) -> int:
        if self._process.stdin is not None and not self._process.stdin.closed:
            try:
                self._process.stdin.close()
            except OSError:
                pass
        remaining = self._deadline - time.monotonic()
        try:
            self._process.wait(timeout=remaining if remaining > 0 else 1)
        except subprocess.TimeoutExpired:
            self._process.kill()
            self._process.wait(timeout=5)
        self._stdout_pump.join(timeout=2)
        self._stderr_pump.join(timeout=2)
        if self._process.stdout is not None:
            self._process.stdout.close()
        if self._process.stderr is not None:
            self._process.stderr.close()
        return self._process.returncode


def exchange(
    session: ProviderSession,
    envelope: dict[str, Any],
    expect_error: bool,
    nested_specs: list[dict[str, Any]] | None = None,
) -> tuple[Any, list[dict[str, Any]]]:
    """Sends one request and returns its validated result-or-error, plus any
    provider/3 events observed along the way.

    A `provider/nestedRequest` frame arriving here is answered inline, in
    the order `nested_specs` declares - this runner playing the host's side
    of docs/provider-protocol.md's nested round trip - and two mismatches are
    both reported as errors rather than silently tolerated: a nested request
    the case did not declare, and a declared one the provider never opened
    before the call's own response arrived.
    """
    session.send(envelope)
    remaining_nested = list(nested_specs or [])
    events: list[dict[str, Any]] = []
    while True:
        message = session.read_message()
        if is_nested_request(message):
            if not remaining_nested:
                raise ConformanceError(
                    f"unexpected provider/nestedRequest during {envelope['method']} (id {envelope['id']})"
                )
            spec = remaining_nested.pop(0)
            nested_id = validate_nested_request(message, spec)
            session.send(build_nested_reply(nested_id, spec))
            continue
        if isinstance(message, dict) and "id" not in message:
            validate_event(message)
            events.append(message)
            continue
        if remaining_nested:
            raise ConformanceError(
                f"{envelope['method']} (id {envelope['id']}) completed with "
                f"{len(remaining_nested)} declared nested request(s) never opened"
            )
        result = validate_envelope(message, envelope["id"], expect_error)
        return result, events


def run_conformance(executable: Path, cases: list[dict[str, Any]], timeout: float) -> dict[str, Any]:
    if not executable.is_absolute() or not executable.is_file() or not os.access(executable, os.X_OK):
        raise ConformanceError("provider must be an absolute executable file")
    session = ProviderSession(executable, timeout)
    events: list[dict[str, Any]] = []
    try:
        describe_result, seen = exchange(session, request(1, "provider/describe"), False)
        events += seen
        activate_result, seen = exchange(session, request(2, "provider/activate"), False)
        events += seen
        _, seen = exchange(session, request(3, "provider/call", {
            "name": "org.maelys.conformance.unknown-tool",
            "arguments": {},
        }), True)
        events += seen
        catalog = validate_description(describe_result)
        if activate_result != {}:
            raise ConformanceError("provider/activate must return an empty object")
        next_id = 4
        results: list[Any] = []
        for case in cases:
            if case["tool"] not in catalog:
                raise ConformanceError(f"case references an unpublished tool: {case['tool']}")
            expect_error = case.get("expect", "success") == "error"
            result, seen = exchange(session, request(next_id, "provider/call", {
                "name": case["tool"],
                "arguments": case.get("arguments", {}),
            }), expect_error, case.get("nested"))
            events += seen
            if not expect_error:
                validate_call_result(result)
                apply_call_assertions(result, case)
            results.append(result)
            next_id += 1
        shutdown_result, seen = exchange(session, request(next_id, "provider/shutdown"), False)
        events += seen
    finally:
        returncode = session.close()
    if returncode != 0:
        raise ConformanceError(f"provider exited with {returncode}: {session.stderr().strip()}")
    if shutdown_result != {}:
        raise ConformanceError("provider/shutdown must return an empty object")
    return {
        "valid": True,
        "provider": describe_result["name"],
        "version": describe_result["version"],
        "toolCount": len(catalog),
        "callCount": len(cases),
        "eventCount": len(events),
        "stderr": session.stderr(),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("provider", type=Path, help="absolute provider executable")
    parser.add_argument("--cases", type=Path, help="optional JSON call scenarios")
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--json", action="store_true")
    arguments = parser.parse_args(argv)
    try:
        report = run_conformance(arguments.provider, read_cases(arguments.cases), arguments.timeout)
    except (ConformanceError, OSError) as error:
        if arguments.json:
            print(json.dumps({"valid": False, "error": str(error)}, ensure_ascii=False, separators=(",", ":")))
        else:
            print(f"provider conformance: FAILED: {error}", file=sys.stderr)
        return 1
    if arguments.json:
        print(json.dumps(report, ensure_ascii=False, separators=(",", ":")))
    else:
        print(f"provider conformance: OK: {report['provider']} {report['version']} ({report['toolCount']} tools)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
