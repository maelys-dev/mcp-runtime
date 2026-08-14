#!/usr/bin/env python3
"""Black-box conformance runner for persistent maelys-provider/2 executables."""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

PROTOCOL = "maelys-provider/2"
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
    if response.get("protocol") != PROTOCOL or response.get("id") != request_id:
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
    return calls


def run_conformance(executable: Path, cases: list[dict[str, Any]], timeout: float) -> dict[str, Any]:
    if not executable.is_absolute() or not executable.is_file() or not os.access(executable, os.X_OK):
        raise ConformanceError("provider must be an absolute executable file")
    requests = [request(1, "provider/describe")]
    expectations: list[bool] = [False]
    requests.append(request(2, "provider/call", {
        "name": "org.maelys.conformance.unknown-tool",
        "arguments": {},
    }))
    expectations.append(True)
    next_id = 3
    for case in cases:
        requests.append(request(next_id, "provider/call", {
            "name": case["tool"],
            "arguments": case.get("arguments", {}),
        }))
        expectations.append(case.get("expect", "success") == "error")
        next_id += 1
    requests.append(request(next_id, "provider/shutdown"))
    expectations.append(False)
    serialized = "".join(json.dumps(item, ensure_ascii=False, separators=(",", ":")) + "\n" for item in requests)
    try:
        completed = subprocess.run(
            [str(executable), "--provider"],
            input=serialized,
            text=True,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise ConformanceError(f"provider exceeded {timeout:g}s") from error
    if completed.returncode != 0:
        raise ConformanceError(f"provider exited with {completed.returncode}: {completed.stderr.strip()}")
    lines = completed.stdout.splitlines()
    if len(lines) != len(requests):
        raise ConformanceError(
            f"provider stdout contains {len(lines)} line(s), expected {len(requests)}; protocol contamination or missing response"
        )
    responses = [load_json(line, f"stdout line {index + 1}") for index, line in enumerate(lines)]
    results = [validate_envelope(response, item["id"], expected) for response, item, expected in zip(responses, requests, expectations)]
    catalog = validate_description(results[0])
    for offset, case in enumerate(cases, start=2):
        if case["tool"] not in catalog:
            raise ConformanceError(f"case references an unpublished tool: {case['tool']}")
        if case.get("expect", "success") == "success":
            validate_call_result(results[offset])
    if results[-1] != {}:
        raise ConformanceError("provider/shutdown must return an empty object")
    return {
        "valid": True,
        "provider": results[0]["name"],
        "version": results[0]["version"],
        "toolCount": len(catalog),
        "callCount": len(cases),
        "stderr": completed.stderr,
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
