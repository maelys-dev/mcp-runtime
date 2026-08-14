import io
import json
import unittest

from maelys_mcp_provider import Tool, create_provider, handle_message, serve_provider, validate_schema_definition


def fixture_provider():
    return create_provider("fixture", "1.0.0", [Tool(
        name="fixture.echo",
        description="Echo a message.",
        input_schema={
            "type": "object",
            "properties": {"message": {"type": "string", "minLength": 1}},
            "required": ["message"],
            "additionalProperties": False,
        },
        output_schema={"type": "object"},
        effect="read",
        handler=lambda arguments: {"message": arguments["message"]},
    )])


class ProviderSdkTest(unittest.TestCase):
    def test_description_omits_handler(self):
        description = fixture_provider().description()
        self.assertEqual(description["name"], "fixture")
        self.assertNotIn("handler", description["tools"][0])

    def test_call_preserves_protocol_and_integer_id(self):
        response = handle_message(fixture_provider(), {
            "protocol": "maelys-provider/1",
            "id": 7,
            "method": "provider/call",
            "params": {"name": "fixture.echo", "arguments": {"message": "hello"}},
        })
        self.assertEqual(response["result"], {"message": "hello"})
        with self.assertRaisesRegex(TypeError, "unsupported provider protocol"):
            handle_message(fixture_provider(), {
                "protocol": "wrong", "id": 8, "method": "provider/describe", "params": {},
            })

    def test_stdio_loop_survives_errors_and_rejects_duplicate_keys(self):
        requests = "\n".join([
            '{"protocol":"maelys-provider/1","id":1,"id":2,"method":"provider/describe","params":{}}',
            json.dumps({"protocol": "maelys-provider/1", "id": 3, "method": "provider/describe", "params": {}}),
            json.dumps({"protocol": "maelys-provider/1", "id": 4, "method": "provider/shutdown", "params": {}}),
        ]) + "\n"
        output = io.StringIO()
        self.assertEqual(serve_provider(fixture_provider(), io.StringIO(requests), output), 0)
        responses = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertIn("duplicate JSON key", responses[0]["error"]["message"])
        self.assertEqual(responses[1]["result"]["name"], "fixture")
        self.assertEqual(responses[2]["result"], {})

    def test_schema_validation_rejects_unsupported_and_ambiguous_shapes(self):
        with self.assertRaisesRegex(TypeError, "unsupported"):
            validate_schema_definition({"type": "object", "oneOf": []})
        with self.assertRaisesRegex(TypeError, "undeclared"):
            validate_schema_definition({"type": "object", "required": ["missing"]})
        with self.assertRaisesRegex(TypeError, "length keywords"):
            validate_schema_definition({"type": "integer", "minLength": 1})
        with self.assertRaisesRegex(TypeError, "numeric bounds"):
            validate_schema_definition({"type": "string", "minimum": 1})
        with self.assertRaisesRegex(TypeError, "does not match"):
            validate_schema_definition({"type": "string", "enum": [1]})
        with self.assertRaisesRegex(TypeError, "duplicate provider tool"):
            tool = fixture_provider().tools[0]
            create_provider("duplicate", "1", [tool, tool])


if __name__ == "__main__":
    unittest.main()
