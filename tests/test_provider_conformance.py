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

    def test_runner_detects_stdout_contamination(self):
        with tempfile.TemporaryDirectory() as directory:
            provider = Path(directory) / "provider"
            provider.write_text(
                "#!/bin/sh\n"
                "while IFS= read -r line; do\n"
                "  echo diagnostic\n"
                "  echo '{\"protocol\":\"maelys-provider/2\",\"id\":1,\"result\":{}}'\n"
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


if __name__ == "__main__":
    unittest.main()
