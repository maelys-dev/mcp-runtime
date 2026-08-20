import io
import json
import unittest

from maelys_mcp_provider import PROTOCOL, PROTOCOL_VERSIONS, CallContext, NestedRequestDenied, NestedRequestError, NestedRequestUnavailable, NestedTransportError, Resource, ResourceTemplate, Tool, complete_result, create_provider, handle_message, input_required_result, resource_result, serve_provider, validate_schema_definition


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
        handler=lambda arguments, _context: complete_result(
            structured_content={"message": arguments["message"]}),
    )])


class ProviderSdkTest(unittest.TestCase):
    def test_description_omits_handler(self):
        description = fixture_provider().description()
        self.assertEqual(description["name"], "fixture")
        self.assertNotIn("handler", description["tools"][0])

    def test_call_preserves_protocol_and_integer_id(self):
        response = handle_message(fixture_provider(), {
            "protocol": "maelys-provider/3",
            "id": 7,
            "method": "provider/call",
            "params": {"name": "fixture.echo", "arguments": {"message": "hello"}},
        })
        self.assertEqual(response["result"], {
            "resultType": "complete", "structuredContent": {"message": "hello"},
        })
        with self.assertRaisesRegex(TypeError, "unsupported provider protocol"):
            handle_message(fixture_provider(), {
                "protocol": "wrong", "id": 8, "method": "provider/describe", "params": {},
            })

    def test_stdio_loop_survives_errors_and_rejects_duplicate_keys(self):
        requests = "\n".join([
            '{"protocol":"maelys-provider/3","id":1,"id":2,"method":"provider/describe","params":{}}',
            json.dumps({"protocol": "maelys-provider/3", "id": 3, "method": "provider/describe", "params": {}}),
            json.dumps({"protocol": "maelys-provider/3", "id": 4, "method": "provider/shutdown", "params": {}}),
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

    def test_mrtr_context_and_result(self):
        observed = []
        def handler(_arguments, context):
            observed.append(context)
            if context.input_responses:
                return complete_result(content=[{"type": "text", "text": "confirmed"}])
            return input_required_result(
                input_requests={"confirm": {"method": "elicitation/create", "params": {"message": "Confirm?"}}},
                request_state="opaque",
            )
        provider = create_provider("mrtr", "1", [Tool(
            name="confirm", description="Confirm", input_schema={"type": "object"},
            effect="apply", handler=handler,
        )])
        first = handle_message(provider, {
            "protocol": "maelys-provider/3", "id": 1, "method": "provider/call",
            "params": {"name": "confirm", "arguments": {}},
        })
        self.assertEqual(first["result"]["resultType"], "input_required")
        second = handle_message(provider, {
            "protocol": "maelys-provider/3", "id": 2, "method": "provider/call",
            "params": {"name": "confirm", "arguments": {},
                "inputResponses": {"confirm": {"action": "accept"}}, "requestState": "opaque"},
        })
        self.assertEqual(second["result"]["resultType"], "complete")
        self.assertEqual(observed[-1].request_state, "opaque")

    def test_resources_are_described_and_read(self):
        provider = create_provider(
            "resources", "1",
            resources=[Resource(uri="fixture://about", name="About", mime_type="text/plain")],
            resource_templates=[ResourceTemplate(uri_template="fixture://echo/{value}", name="Echo")],
            read_resource=lambda uri, _context: resource_result([
                {"uri": uri, "mimeType": "text/plain", "text": f"read {uri}"},
            ]),
        )
        description = provider.description()
        self.assertNotIn("size", description["resources"][0])
        self.assertEqual(description["resourceTemplates"][0]["uriTemplate"], "fixture://echo/{value}")
        response = handle_message(provider, {
            "protocol": "maelys-provider/3", "id": 10,
            "method": "provider/readResource", "params": {"uri": "fixture://echo/hello"},
        })
        self.assertEqual(response["result"], {"resultType": "complete", "contents": [{
            "uri": "fixture://echo/hello", "mimeType": "text/plain",
            "text": "read fixture://echo/hello",
        }]})

    def test_provider_3_serializes_events_with_responses(self):
        provider = None

        def emit_events(_arguments, _context):
            provider.events.resource_updated("fixture://course/one")
            provider.events.resources_list_changed()
            provider.events.tools_list_changed()
            return complete_result(structured_content={"emitted": 3})

        provider = create_provider("events", "1", [Tool(
            name="events.emit", description="Emit all event shapes.",
            input_schema={"type": "object"}, effect="execute",
            handler=emit_events,
        )])
        with self.assertRaisesRegex(RuntimeError, "before activation"):
            provider.events.tools_list_changed()
        requests = "\n".join(json.dumps(message) for message in [
            {"protocol": "maelys-provider/3", "id": 1,
                "method": "provider/activate", "params": {}},
            {"protocol": "maelys-provider/3", "id": 2,
                "method": "provider/call",
                "params": {"name": "events.emit", "arguments": {}}},
            {"protocol": "maelys-provider/3", "id": 3,
                "method": "provider/shutdown", "params": {}},
        ]) + "\n"
        output = io.StringIO()
        self.assertEqual(serve_provider(provider, io.StringIO(requests), output), 0)
        messages = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual([
            message.get("method", f"response:{message.get('id')}")
            for message in messages
        ], [
            "response:1",
            "provider/notifications/resources/updated",
            "provider/notifications/resources/list_changed",
            "provider/notifications/tools/list_changed",
            "response:2",
            "response:3",
        ])
        self.assertEqual(messages[1]["params"]["uri"], "fixture://course/one")
        with self.assertRaisesRegex(RuntimeError, "after shutdown"):
            provider.events.tools_list_changed()


def nesting_provider(handler, read_resource=None):
    """A provider whose one tool does whatever the test's handler does."""
    return create_provider(
        "nested", "1",
        [Tool(name="nested.ask", description="Ask the client something.",
            input_schema={"type": "object"}, effect="read", handler=handler)],
        resources=[] if read_resource is None else [
            Resource(uri="nested://ask", name="Ask")],
        read_resource=read_resource,
    )


def request_line(request_id, method, params, protocol="maelys-provider/3"):
    return json.dumps({"protocol": protocol, "id": request_id,
        "method": method, "params": params})


def call_line(request_id, arguments=None):
    return request_line(request_id, "provider/call",
        {"name": "nested.ask", "arguments": arguments or {}})


def reply_line(nested_id="n1", result=None, error=None, protocol=PROTOCOL):
    params = {"nestedId": nested_id}
    if result is not None:
        params["result"] = result
    if error is not None:
        params["error"] = error
    return json.dumps({"protocol": protocol,
        "method": "provider/nestedReply", "params": params})


def run_session(provider, lines, source=None):
    """One serve_provider run over a scripted stdin, as parsed frames out."""
    output = io.StringIO()
    stream = source if source is not None else io.StringIO(
        "".join(f"{line}\n" for line in lines))
    status = serve_provider(provider, stream, output)
    assert status == 0
    return [json.loads(line) for line in output.getvalue().splitlines()]


class NestedRequestTest(unittest.TestCase):
    def test_elicitation_round_trip(self):
        answers = []

        def handler(_arguments, context):
            answers.append(context.request_elicitation({"message": "Proceed?"}))
            return complete_result(content=[
                {"type": "text", "text": answers[-1]["answer"]}])

        messages = run_session(nesting_provider(handler), [
            call_line(1),
            reply_line("n1", result={"answer": "yes"}),
        ])
        self.assertEqual(messages[0], {
            "protocol": "maelys-provider/5",
            "method": "provider/nestedRequest",
            "params": {"nestedId": "n1", "method": "elicitation/create",
                "params": {"message": "Proceed?"}},
        })
        self.assertEqual(answers, [{"answer": "yes"}])
        # The call's own response declares the version its nested request
        # already announced, rather than talking the host back down.
        self.assertEqual(messages[1]["protocol"], "maelys-provider/5")
        self.assertEqual(messages[1]["result"]["content"][0]["text"], "yes")

    def test_sampling_and_roots_carry_their_own_methods_and_ids(self):
        def handler(arguments, context):
            answer = context.request_roots() if arguments.get("what") == "roots" \
                else context.request_sampling({"maxTokens": 1})
            return complete_result(structured_content=answer)

        messages = run_session(nesting_provider(handler), [
            call_line(1, {"what": "sampling"}),
            reply_line("n1", result={"role": "assistant"}),
            call_line(2, {"what": "roots"}),
            reply_line("n2", result={"roots": []}),
        ])
        self.assertEqual(
            [message["params"]["method"] for message in messages
                if message.get("method") == "provider/nestedRequest"],
            ["sampling/createMessage", "roots/list"])
        # A second request never reuses the id the first was answered on, and
        # roots/list sends the empty params object rather than omitting it.
        self.assertEqual(messages[2]["params"]["nestedId"], "n2")
        self.assertEqual(messages[2]["params"]["params"], {})
        self.assertEqual(messages[3]["result"]["structuredContent"], {"roots": []})

    def test_a_resource_handler_may_nest_too(self):
        def read_resource(uri, context):
            answer = context.request_elicitation({"message": uri})
            return resource_result([{"uri": uri, "text": answer["answer"]}])

        provider = nesting_provider(
            lambda _arguments, _context: complete_result(structured_content={}),
            read_resource=read_resource)
        messages = run_session(provider, [
            request_line(1, "provider/readResource", {"uri": "nested://ask"}),
            reply_line("n1", result={"answer": "read"}),
        ])
        self.assertEqual(messages[0]["params"]["params"], {"message": "nested://ask"})
        self.assertEqual(messages[1]["result"]["contents"][0]["text"], "read")

    def test_a_refused_request_raises_the_hosts_code(self):
        seen = []

        def handler(_arguments, context):
            try:
                context.request_elicitation({"message": "Proceed?"})
            except NestedRequestError as error:
                seen.append((type(error), error.code, str(error), error.data))
                return complete_result(content=[{"type": "text", "text": error.code}])
            return complete_result(content=[{"type": "text", "text": "answered"}])

        messages = run_session(nesting_provider(handler), [
            call_line(1),
            reply_line("n1", error={"code": "denied",
                "message": "the client did not declare elicitation"}),
            call_line(2),
            # The client's own JSON-RPC error travels as data, so a handler can
            # tell "the user declined" from "the request never got there".
            reply_line("n2", error={"code": "client_error", "message": "rejected",
                "data": {"code": -32603, "message": "no"}}),
        ])
        self.assertEqual(seen, [
            (NestedRequestDenied, "denied",
                "the client did not declare elicitation", None),
            (NestedRequestError, "client_error", "rejected",
                {"code": -32603, "message": "no"}),
        ])
        self.assertEqual(messages[1]["result"]["content"][0]["text"], "denied")
        self.assertEqual(messages[3]["result"]["content"][0]["text"], "client_error")

    def test_a_reply_interleaved_with_ordinary_frames_loses_no_frame(self):
        def handler(_arguments, context):
            answer = context.request_elicitation({"message": "Proceed?"})
            return complete_result(content=[
                {"type": "text", "text": answer["answer"]}])

        messages = run_session(nesting_provider(handler), [
            call_line(1),
            # Arrives while the handler is blocked. It is not the handler's to
            # answer, so it waits for the dispatch loop rather than being
            # consumed by the wait or answered out of turn.
            request_line(2, "provider/describe", {}),
            reply_line("n1", result={"answer": "late"}),
            request_line(3, "provider/shutdown", {}),
        ])
        self.assertEqual([message.get("id", message.get("method")) for message in messages],
            ["provider/nestedRequest", 1, 2, 3])
        self.assertEqual(messages[1]["result"]["content"][0]["text"], "late")
        self.assertEqual(messages[2]["result"]["name"], "nested")

    def test_a_malformed_reply_fails_the_call(self):
        seen = []

        def handler(_arguments, context):
            try:
                return complete_result(structured_content=context.request_elicitation({}))
            except NestedTransportError as error:
                seen.append((error.code, str(error)))
                raise

        # A session each: the reply is wrong in a different way every time, and
        # a wire that has already gone wrong cannot host the next case.
        uncorrelated = run_session(nesting_provider(handler), [
            call_line(1), reply_line("n7", result={"answer": "stale"})])
        empty = run_session(nesting_provider(handler), [
            call_line(1), reply_line("n1")])
        both = run_session(nesting_provider(handler), [
            call_line(1), reply_line("n1", result={}, error={"code": "denied"})])
        self.assertEqual([code for code, _ in seen], ["malformed"] * 3)
        self.assertIn("does not correlate", seen[0][1])
        self.assertIn("exactly one result or error", seen[1][1])
        self.assertIn("exactly one result or error", seen[2][1])
        for messages in (uncorrelated, empty, both):
            self.assertEqual(messages[1]["error"]["code"], "provider_error")

    def test_a_transport_that_closes_mid_wait_fails_the_call(self):
        seen = []

        def handler(_arguments, context):
            try:
                return complete_result(structured_content=context.request_sampling({}))
            except NestedTransportError as error:
                seen.append(error.code)
                raise

        # Nothing follows the call: the host went away rather than answering.
        messages = run_session(nesting_provider(handler), [call_line(1)])
        self.assertEqual(seen, ["closed"])
        self.assertEqual(messages[1]["id"], 1)
        self.assertIn("closed before", messages[1]["error"]["message"])

    def test_a_second_request_while_one_is_outstanding_is_refused(self):
        class ReentrantSource:
            """Re-enters the handler from inside readline, so "one at a time"
            is decided by the guard rather than by a second thread's timing."""

            def __init__(self, lines):
                self._lines = list(lines)
                self.context = None
                self.refusal = None

            def readline(self):
                if self.context is not None and self.refusal is None:
                    try:
                        self.context.request_roots()
                        self.refusal = "not refused"
                    except NestedRequestUnavailable as error:
                        self.refusal = str(error)
                return self._lines.pop(0) if self._lines else ""

        source = ReentrantSource([f"{call_line(1)}\n",
            f"{reply_line('n1', result={'answer': 'yes'})}\n"])

        def handler(_arguments, context):
            source.context = context
            return complete_result(structured_content=context.request_elicitation({}))

        messages = run_session(nesting_provider(handler), [], source=source)
        self.assertEqual(source.refusal, "this call already has a nested request outstanding")
        self.assertEqual(messages[1]["result"]["structuredContent"], {"answer": "yes"})

    def test_nested_requests_are_scoped_to_the_call_they_belong_to(self):
        stashed = []

        def handler(_arguments, context):
            stashed.append(context)
            return complete_result(structured_content={})

        run_session(nesting_provider(handler), [call_line(1)])
        # The host fails the whole transport for a nested request sent while no
        # call is in flight, so a stashed context has to fail on this side.
        with self.assertRaisesRegex(NestedRequestUnavailable, "inside the call"):
            stashed[0].request_elicitation({})
        # And a context with no transport at all - handle_message on its own -
        # says so rather than pretending it wrote something.
        with self.assertRaisesRegex(NestedRequestUnavailable, "serve_provider transport"):
            CallContext().request_sampling({})

    def test_a_provider_that_never_nests_declares_the_older_version(self):
        messages = run_session(fixture_provider(), [
            request_line(1, "provider/describe", {}),
            request_line(2, "provider/activate", {}),
            json.dumps({"protocol": "maelys-provider/3", "id": 3,
                "method": "provider/call", "params": {"name": "fixture.echo",
                    "arguments": {"message": "hello"}}}),
            request_line(4, "provider/shutdown", {}),
        ])
        # Spelled out rather than compared against PROTOCOL_DECLARED: the
        # property under test is that these bytes did not change when the SDK
        # learned /5, and a constant would move with the code.
        self.assertEqual({message["protocol"] for message in messages},
            {"maelys-provider/4"})
        self.assertEqual(PROTOCOL, "maelys-provider/5")
        self.assertEqual(handle_message(fixture_provider(), {
            "protocol": "maelys-provider/3", "id": 5,
            "method": "provider/describe", "params": {},
        })["protocol"], "maelys-provider/4")

    def test_every_version_in_the_range_is_valid_inbound(self):
        # The host addresses a provider at whatever it last declared, and this
        # SDK declares /4 until it nests - so /4 has to be accepted even though
        # it is neither the floor nor the newest version.
        for index, protocol in enumerate(PROTOCOL_VERSIONS):
            response = handle_message(fixture_provider(), {
                "protocol": protocol, "id": index,
                "method": "provider/describe", "params": {},
            })
            self.assertEqual(response["id"], index)
        for protocol in ("maelys-provider/2", "maelys-provider/6"):
            with self.assertRaisesRegex(TypeError, "unsupported provider protocol"):
                handle_message(fixture_provider(), {"protocol": protocol,
                    "id": 1, "method": "provider/describe", "params": {}})


if __name__ == "__main__":
    unittest.main()
