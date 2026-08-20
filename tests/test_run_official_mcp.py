import importlib.util
import json
import stat
import tempfile
import threading
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[1] / "conformance" / "run_official_mcp.py"
SPEC = importlib.util.spec_from_file_location("run_official_mcp", MODULE_PATH)
assert SPEC and SPEC.loader
run_official_mcp = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(run_official_mcp)


def _write_nested_runtime(directory: str) -> Path:
    """A dependency-free stand-in for the real maelys-mcp binary: on a
    `tools/call` request it opens exactly one server-initiated NESTED
    request (an `elicitation/create` with its own id, per
    docs/architecture.md "Nested requests"), blocks for the reply on the
    same stdin the outer request arrived on, then completes the call.

    Used to unit-test StdioBridge.stream()/send() at the mechanism level,
    without the real official npx client or a built runtime binary - the
    slow, network-gated path `make test-mcp-conformance-official` covers.
    """
    runtime = Path(directory) / "nested-runtime"
    runtime.write_text(
        "#!/usr/bin/env python3\n"
        "import json, sys\n"
        "def send(message):\n"
        "    sys.stdout.write(json.dumps(message) + chr(10))\n"
        "    sys.stdout.flush()\n"
        "for line in sys.stdin:\n"
        "    message = json.loads(line)\n"
        "    if message.get('method') == 'tools/call' and 'id' in message:\n"
        "        send({'jsonrpc': '2.0', 'id': 'nested-1',\n"
        "            'method': 'elicitation/create', 'params': {}})\n"
        "        reply = json.loads(sys.stdin.readline())\n"
        "        answer = reply.get('result', {}).get('answer')\n"
        "        send({'jsonrpc': '2.0', 'id': message['id'], 'result': {'echo': answer}})\n"
        "    elif 'id' in message:\n"
        "        send({'jsonrpc': '2.0', 'id': message['id'], 'result': {}})\n",
        encoding="utf-8",
    )
    runtime.chmod(runtime.stat().st_mode | stat.S_IXUSR)
    return runtime.resolve()


class ExpectsReplyTest(unittest.TestCase):
    def test_classifies_requests_notifications_and_plain_responses(self):
        self.assertTrue(run_official_mcp._expects_reply(
            json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {}}).encode()))
        self.assertFalse(run_official_mcp._expects_reply(
            json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}}).encode()))
        # A client's reply to a server-initiated nested request: has an id,
        # no method - the exact shape that used to hang StdioBridge.request().
        self.assertFalse(run_official_mcp._expects_reply(
            json.dumps({"jsonrpc": "2.0", "id": "nested-1", "result": {"answer": "yes"}}).encode()))
        self.assertFalse(run_official_mcp._expects_reply(b"not json"))


class StdioBridgeNestedTest(unittest.TestCase):
    def test_stream_delivers_the_nested_request_before_send_answers_it(self):
        """The regression this guards: StdioBridge used to buffer every line
        for a request internally and return them only once the exchange was
        fully complete, and separately would block a reply's own send()
        behind whatever call was still being collected. Together those two
        bugs meant a real HTTP client was never shown the nested request it
        was meant to answer, and its answer could not get forwarded even if
        it somehow had one - producing the official conformance suite's
        "-32001: Request timed out" on tools-call-sampling/tools-call-elicitation
        before this fix. Reproduced here with two real OS threads, exactly
        the shape two concurrent HTTP handler threads (the open tools/call
        POST and the reply POST) drive it in production.
        """
        with tempfile.TemporaryDirectory() as directory:
            runtime = _write_nested_runtime(directory)
            bridge = run_official_mcp.StdioBridge([str(runtime)])
            try:
                call = json.dumps({
                    "jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {},
                }).encode()
                generator = bridge.stream(call)

                first_line: list[bytes] = []
                first_seen = threading.Event()

                def drive_stream() -> None:
                    line, is_final = next(generator)
                    first_line.append(line)
                    self.assertFalse(is_final)
                    first_seen.set()

                stream_thread = threading.Thread(target=drive_stream)
                stream_thread.start()
                self.assertTrue(first_seen.wait(timeout=5), "nested request was never streamed")
                stream_thread.join(timeout=5)
                self.assertFalse(stream_thread.is_alive())

                nested = json.loads(first_line[0])
                self.assertEqual(nested["method"], "elicitation/create")

                # The reply POST: sent from a second thread, exactly like a
                # real second HTTP connection would - must not block behind
                # the still-open `stream()` call above.
                reply = json.dumps({
                    "jsonrpc": "2.0", "id": nested["id"], "result": {"answer": "yes"},
                }).encode()
                reply_thread = threading.Thread(target=bridge.send, args=(reply,))
                reply_thread.start()
                reply_thread.join(timeout=5)
                self.assertFalse(reply_thread.is_alive(), "send() blocked behind the open stream")

                final_line, is_final = next(generator)
                self.assertTrue(is_final)
                self.assertEqual(json.loads(final_line), {
                    "jsonrpc": "2.0", "id": 1, "result": {"echo": "yes"},
                })
                with self.assertRaises(StopIteration):
                    next(generator)
            finally:
                bridge.close()


if __name__ == "__main__":
    unittest.main()
