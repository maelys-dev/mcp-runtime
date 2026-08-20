import importlib.util
import json
import os
import stat
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[1] / "conformance" / "provider_conformance.py"
SPEC = importlib.util.spec_from_file_location("provider_conformance", MODULE_PATH)
assert SPEC and SPEC.loader
provider_conformance = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(provider_conformance)


class ProviderConformanceTest(unittest.TestCase):
    def test_schema_definition_rejects_unsupported_or_ambiguous_shapes(self):
        valid = {
            "type": "object",
            "properties": {"name": {"type": "string", "minLength": 1}},
            "required": ["name"],
            "additionalProperties": False,
        }
        provider_conformance.validate_schema(valid, "schema")
        with self.assertRaisesRegex(provider_conformance.ConformanceError, "unsupported"):
            provider_conformance.validate_schema({"type": "object", "oneOf": []}, "schema")
        with self.assertRaisesRegex(provider_conformance.ConformanceError, "undeclared"):
            provider_conformance.validate_schema({"type": "object", "required": ["name"]}, "schema")
        with self.assertRaisesRegex(provider_conformance.ConformanceError, "length keywords"):
            provider_conformance.validate_schema({"type": "integer", "minLength": 1}, "schema")
        with self.assertRaisesRegex(provider_conformance.ConformanceError, "numeric bounds"):
            provider_conformance.validate_schema({"type": "string", "minimum": 1}, "schema")
        with self.assertRaisesRegex(provider_conformance.ConformanceError, "does not match"):
            provider_conformance.validate_schema({"type": "string", "enum": [1]}, "schema")

    def test_strict_json_rejects_duplicate_keys(self):
        with self.assertRaisesRegex(provider_conformance.ConformanceError, "duplicate JSON key"):
            provider_conformance.load_json('{"id":1,"id":2}', "message")

    def test_provider_3_event_shapes_are_strict(self):
        provider_conformance.validate_event({
            "protocol": "maelys-provider/3",
            "method": "provider/notifications/resources/updated",
            "params": {"uri": "fixture://course"},
        })
        with self.assertRaisesRegex(provider_conformance.ConformanceError, "non-empty uri"):
            provider_conformance.validate_event({
                "protocol": "maelys-provider/3",
                "method": "provider/notifications/resources/updated",
                "params": {},
            })
        with self.assertRaisesRegex(provider_conformance.ConformanceError, "unsupported"):
            provider_conformance.validate_event({
                "protocol": "maelys-provider/3",
                "method": "provider/notifications/unknown",
                "params": {},
            })

    def test_runner_detects_stdout_contamination(self):
        with tempfile.TemporaryDirectory() as directory:
            provider = Path(directory) / "provider"
            provider.write_text(
                "#!/bin/sh\n"
                "while IFS= read -r line; do\n"
                "  echo diagnostic\n"
                "  echo '{\"protocol\":\"maelys-provider/3\",\"id\":1,\"result\":{}}'\n"
                "done\n",
                encoding="utf-8",
            )
            provider.chmod(provider.stat().st_mode | stat.S_IXUSR)
            with self.assertRaisesRegex(provider_conformance.ConformanceError, "protocol contamination"):
                provider_conformance.run_conformance(provider.resolve(), [], 10)

    def test_case_file_is_strict_and_typed(self):
        with tempfile.TemporaryDirectory() as directory:
            case_file = Path(directory) / "cases.json"
            case_file.write_text(json.dumps({
                "calls": [{"tool": "example.echo", "arguments": {"root": "${CONFORMANCE_ROOT}"}}],
            }), encoding="utf-8")
            previous = os.environ.get("CONFORMANCE_ROOT")
            os.environ["CONFORMANCE_ROOT"] = directory
            try:
                case = provider_conformance.read_cases(case_file)[0]
            finally:
                if previous is None:
                    os.environ.pop("CONFORMANCE_ROOT", None)
                else:
                    os.environ["CONFORMANCE_ROOT"] = previous
            self.assertEqual(case["tool"], "example.echo")
            self.assertEqual(case["arguments"]["root"], directory)

    def test_case_file_rejects_malformed_nested_spec(self):
        with tempfile.TemporaryDirectory() as directory:
            case_file = Path(directory) / "cases.json"
            case_file.write_text(json.dumps({
                "calls": [{"tool": "nested.echo", "nested": [
                    {"method": "elicitation/create", "result": {}, "error": {"code": "denied", "message": "no"}},
                ]}],
            }), encoding="utf-8")
            with self.assertRaisesRegex(provider_conformance.ConformanceError, "exactly one of result or error"):
                provider_conformance.read_cases(case_file)

    def test_validate_nested_request_checks_shape_and_declared_method(self):
        message = {
            "protocol": "maelys-provider/5", "method": "provider/nestedRequest",
            "params": {"nestedId": "n1", "method": "elicitation/create", "params": {}},
        }
        self.assertEqual(
            provider_conformance.validate_nested_request(message, {"method": "elicitation/create"}), "n1")
        # No declared method in the spec: any method is accepted.
        self.assertEqual(provider_conformance.validate_nested_request(message, {}), "n1")
        with self.assertRaisesRegex(provider_conformance.ConformanceError, "method mismatch"):
            provider_conformance.validate_nested_request(message, {"method": "sampling/createMessage"})
        with self.assertRaisesRegex(provider_conformance.ConformanceError, "non-empty nestedId"):
            provider_conformance.validate_nested_request({
                "protocol": "maelys-provider/5", "method": "provider/nestedRequest",
                "params": {"method": "elicitation/create", "params": {}},
            }, {})

    def test_build_nested_reply_shapes_result_and_error(self):
        result_reply = provider_conformance.build_nested_reply("n1", {"result": {"answer": "yes"}})
        self.assertEqual(result_reply["method"], "provider/nestedReply")
        self.assertEqual(result_reply["params"], {"nestedId": "n1", "result": {"answer": "yes"}})
        error_reply = provider_conformance.build_nested_reply(
            "n1", {"error": {"code": "denied", "message": "no"}})
        self.assertEqual(error_reply["params"], {"nestedId": "n1", "error": {"code": "denied", "message": "no"}})

    def test_apply_call_assertions_content_and_structured(self):
        provider_conformance.apply_call_assertions(
            {"content": [{"type": "text", "text": "yes"}]}, {"assertContentText": "yes"})
        with self.assertRaisesRegex(provider_conformance.ConformanceError, "content\\[0\\]\\.text"):
            provider_conformance.apply_call_assertions(
                {"content": [{"type": "text", "text": "no"}]}, {"assertContentText": "yes"})
        provider_conformance.apply_call_assertions(
            {"structuredContent": {"ok": True}}, {"assertStructuredContent": {"ok": True}})
        with self.assertRaisesRegex(provider_conformance.ConformanceError, "structuredContent"):
            provider_conformance.apply_call_assertions(
                {"structuredContent": {"ok": False}}, {"assertStructuredContent": {"ok": True}})

    def _write_nested_fixture(self, directory: str) -> Path:
        """A dependency-free provider/5 fixture that opens exactly one nested
        elicitation/create request on `nested.echo`, then echoes back
        whatever `answer` the reply carried - used to unit-test exchange()'s
        own nested handling without needing a built SDK provider."""
        provider = Path(directory) / "nested-fixture"
        provider.write_text(
            "#!/usr/bin/env python3\n"
            "import json, sys\n"
            "def send(message):\n"
            "    sys.stdout.write(json.dumps(message) + chr(10))\n"
            "    sys.stdout.flush()\n"
            "for line in sys.stdin:\n"
            "    message = json.loads(line)\n"
            "    method, mid = message.get('method'), message.get('id')\n"
            "    if method == 'provider/describe':\n"
            "        send({'protocol': 'maelys-provider/3', 'id': mid, 'result': {\n"
            "            'name': 'nested-fixture', 'version': '1.0.0', 'tools': [{\n"
            "                'name': 'nested.echo', 'description': 'echoes a nested reply',\n"
            "                'inputSchema': {'type': 'object', 'properties': {}, 'additionalProperties': False},\n"
            "                'effect': 'read'}, {\n"
            "                'name': 'plain.echo', 'description': 'never nests',\n"
            "                'inputSchema': {'type': 'object', 'properties': {}, 'additionalProperties': False},\n"
            "                'effect': 'read'}]}})\n"
            "    elif method == 'provider/activate':\n"
            "        send({'protocol': 'maelys-provider/3', 'id': mid, 'result': {}})\n"
            "    elif method == 'provider/call' and message['params']['name'] == 'nested.echo':\n"
            "        send({'protocol': 'maelys-provider/5', 'method': 'provider/nestedRequest',\n"
            "            'params': {'nestedId': 'n1', 'method': 'elicitation/create', 'params': {}}})\n"
            "        reply = json.loads(sys.stdin.readline())['params']\n"
            "        answer = reply.get('result', {}).get('answer', '') if 'result' in reply else 'denied'\n"
            "        send({'protocol': 'maelys-provider/5', 'id': mid, 'result': {\n"
            "            'resultType': 'complete', 'content': [{'type': 'text', 'text': answer}]}})\n"
            "    elif method == 'provider/call' and message['params']['name'] == 'plain.echo':\n"
            "        send({'protocol': 'maelys-provider/3', 'id': mid, 'result': {\n"
            "            'resultType': 'complete', 'content': [{'type': 'text', 'text': 'plain'}]}})\n"
            "    elif method == 'provider/call':\n"
            "        send({'protocol': 'maelys-provider/3', 'id': mid,\n"
            "            'error': {'code': 'not_found', 'message': 'unknown tool'}})\n"
            "    elif method == 'provider/shutdown':\n"
            "        send({'protocol': 'maelys-provider/3', 'id': mid, 'result': {}})\n"
            "        break\n",
            encoding="utf-8",
        )
        provider.chmod(provider.stat().st_mode | stat.S_IXUSR)
        return provider.resolve()

    def test_nested_round_trip_happy_path(self):
        with tempfile.TemporaryDirectory() as directory:
            provider = self._write_nested_fixture(directory)
            report = provider_conformance.run_conformance(provider, [{
                "tool": "nested.echo", "arguments": {}, "expect": "success",
                "nested": [{"method": "elicitation/create", "result": {"answer": "yes"}}],
                "assertContentText": "yes",
            }], 10)
            self.assertTrue(report["valid"])

    def test_nested_round_trip_fails_red_when_declared_method_is_wrong(self):
        """The red half of the round-trip test above: a case that declares
        the wrong nested method must fail loudly, not silently pass because
        some nested request happened to arrive."""
        with tempfile.TemporaryDirectory() as directory:
            provider = self._write_nested_fixture(directory)
            with self.assertRaisesRegex(provider_conformance.ConformanceError, "method mismatch"):
                provider_conformance.run_conformance(provider, [{
                    "tool": "nested.echo", "arguments": {}, "expect": "success",
                    "nested": [{"method": "sampling/createMessage", "result": {"answer": "yes"}}],
                }], 10)

    def test_nested_round_trip_fails_when_provider_never_opens_declared_nested_request(self):
        with tempfile.TemporaryDirectory() as directory:
            provider = self._write_nested_fixture(directory)
            with self.assertRaisesRegex(provider_conformance.ConformanceError, "never opened"):
                provider_conformance.run_conformance(provider, [{
                    "tool": "plain.echo", "arguments": {}, "expect": "success",
                    "nested": [{"method": "elicitation/create", "result": {}}],
                }], 10)


if __name__ == "__main__":
    unittest.main()
