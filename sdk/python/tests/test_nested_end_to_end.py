"""The Python SDK's nested round trip through the real host, over real stdio.

Everything else in this directory drives serve_provider over a string: fast,
and blind to the half of the contract that only exists between two processes.
Here the host binary is the real one, it spawns the fixture provider itself
with the environment it really uses, and the client on the other end is this
test - so what is checked is the wire, not a mock of it.

Skipped when the runtime has not been built. `make test-sdk-nested` sets
MAELYS_MCP_BINARY after building it, which turns the skip into a hard failure:
a gate that can silently pass by skipping is not a gate.
"""

from __future__ import annotations

import json
import os
import selectors
import subprocess
import time
import unittest
from pathlib import Path

LEGACY_PROTOCOL = "2025-11-25"
ROOT = Path(__file__).resolve().parents[3]
FIXTURE = Path(__file__).resolve().parent / "nested_fixture_provider.py"
DEFAULT_BINARY = ROOT / "build" / "release" / "bin" / "maelys-mcp"
REQUIRED_BINARY = os.environ.get("MAELYS_MCP_BINARY")
BINARY = Path(REQUIRED_BINARY) if REQUIRED_BINARY else DEFAULT_BINARY


class Client:
    """A hand-rolled MCP client on the host's stdio, deliberately not the
    runtime's own machinery: the point is to be the other end, not to share
    an implementation with it.

    Unbuffered, with the line splitting done here. A buffered reader would let
    a chunk carrying two frames leave the second one sitting in the buffer
    while select() reports nothing to read - a deadline failure with no cause
    visible anywhere, and exactly the kind of flake a timing test must not
    have.
    """

    def __init__(self) -> None:
        self._process = subprocess.Popen(
            [str(BINARY), "--provider", str(FIXTURE)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        self._output = self._process.stdout.fileno()
        self._buffer = b""
        self._selector = selectors.DefaultSelector()
        self._selector.register(self._output, selectors.EVENT_READ)

    def send(self, message: dict) -> None:
        self._process.stdin.write(json.dumps(message).encode() + b"\n")

    def next_frame(self, timeout: float = 10.0) -> dict:
        deadline = time.monotonic() + timeout
        while True:
            newline = self._buffer.find(b"\n")
            if newline >= 0:
                line = self._buffer[:newline]
                self._buffer = self._buffer[newline + 1:]
                if line.strip():
                    return json.loads(line)
                continue
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not self._selector.select(remaining):
                raise AssertionError("the host sent nothing before the deadline")
            chunk = os.read(self._output, 65536)
            if not chunk:
                raise AssertionError("the host closed stdout")
            self._buffer += chunk

    def frame_with_id(self, wanted, timeout: float = 10.0) -> dict:
        """The frame carrying this id, skipping whatever came first - a nested
        request and a response are both legal here, in either order."""
        deadline = time.monotonic() + timeout
        while True:
            frame = self.next_frame(max(deadline - time.monotonic(), 0.0))
            if frame.get("id") == wanted:
                return frame

    def handshake(self, capabilities: dict) -> None:
        self.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": LEGACY_PROTOCOL, "capabilities": capabilities,
            "clientInfo": {"name": "python-sdk-nested-test", "version": "1"}}})
        self.frame_with_id(1)
        self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def close(self) -> None:
        self._selector.close()
        if self._process.stdin and not self._process.stdin.closed:
            self._process.stdin.close()
        try:
            self._process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self._process.kill()
            self._process.wait(timeout=5)
        if self._process.stdout and not self._process.stdout.closed:
            self._process.stdout.close()


# Skipping is for an unbuilt tree only. A caller that named a binary gets the
# failure rather than the skip, even if the path it named is not there.
@unittest.skipUnless(REQUIRED_BINARY or BINARY.exists(),
    f"{BINARY} is not built; run make, or make test-sdk-nested")
class NestedEndToEndTest(unittest.TestCase):
    def setUp(self) -> None:
        self.client = Client()
        self.addCleanup(self.client.close)

    def test_a_nested_elicitation_completes_through_the_real_host(self):
        self.client.handshake({"elicitation": {}})
        self.client.send({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
            "params": {"name": "nested.ask", "arguments": {"message": "Proceed?"}}})

        nested = self.client.next_frame()
        # A real server-to-client request: the host's own id, the method the
        # provider named, and the params the SDK helper built.
        self.assertEqual(nested["method"], "elicitation/create")
        self.assertIsInstance(nested["id"], str)
        self.assertEqual(nested["params"]["message"], "Proceed?")
        self.assertIn("answer", nested["params"]["requestedSchema"]["properties"])

        self.client.send({"jsonrpc": "2.0", "id": nested["id"],
            "result": {"answer": "yes"}})
        response = self.client.frame_with_id(2)
        self.assertNotIn("error", response)
        self.assertNotEqual(response["result"].get("isError"), True)
        self.assertEqual(response["result"]["content"][0]["text"], "yes")

    def test_an_undeclared_capability_is_refused_before_it_is_sent(self):
        self.client.handshake({})
        self.client.send({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
            "params": {"name": "nested.ask", "arguments": {}}})
        # The first frame back is the response, not an elicitation request:
        # nothing was sent to a client that never offered the surface, and the
        # SDK raised the host's own code rather than a generic failure.
        frame = self.client.next_frame()
        self.assertEqual(frame.get("id"), 2)
        self.assertEqual(frame["result"]["content"][0]["text"], "error:denied")


if __name__ == "__main__":
    unittest.main()
