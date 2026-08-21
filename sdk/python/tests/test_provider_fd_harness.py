"""A scripted fd-3 harness for MAELYS_PROVIDER_FD (docs/launch-contract-
design.md, "The child's descriptors"), at the Python SDK's own test layer -
no real host, no C helper. socket.socketpair() gives a genuine duplex AF_UNIX
socket, the same substrate the real POSIX launcher hands a native provider on
fd 3 under MAELYS_MCP_PROCESS_FD_ISOLATED; pass_fds keeps the child's end open
across exec, and MAELYS_PROVIDER_FD is set to whatever descriptor number it
actually landed on rather than hardcoding 3 - that number is a launcher
convention this harness does not need to reproduce, only the adoption
mechanism the SDK implements around it.

fixture_provider.py takes no fd arguments of its own - it just calls
serve_provider(provider) - so if it adopts MAELYS_PROVIDER_FD at all, it is
doing so unprompted by anything test-specific, the same as a real provider
process would.
"""
from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import unittest
from pathlib import Path

FIXTURE = str(Path(__file__).resolve().parent / "fixture_provider.py")
SDK_SRC = str(Path(__file__).resolve().parent.parent / "src")


def _env_with_sdk_on_path() -> dict:
    env = dict(os.environ)
    existing = env.get("PYTHONPATH")
    env["PYTHONPATH"] = f"{SDK_SRC}{os.pathsep}{existing}" if existing else SDK_SRC
    return env


class ProviderFdHarnessTest(unittest.TestCase):
    def test_real_duplex_fd_carries_a_full_session(self) -> None:
        parent_sock, child_sock = socket.socketpair()
        try:
            os.set_inheritable(child_sock.fileno(), True)
            env = _env_with_sdk_on_path()
            env["MAELYS_PROVIDER_FD"] = str(child_sock.fileno())
            process = subprocess.Popen(
                [sys.executable, FIXTURE],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=env,
                pass_fds=(child_sock.fileno(),),
            )
            child_sock.close()  # the parent only ever talks over parent_sock

            reader = parent_sock.makefile("r", encoding="utf-8")

            def send(message: dict) -> None:
                parent_sock.sendall((json.dumps(message) + "\n").encode("utf-8"))

            send({"protocol": "maelys-provider/3", "id": 1, "method": "provider/describe", "params": {}})
            describe_response = json.loads(reader.readline())
            self.assertEqual(describe_response["result"]["name"], "python-fixture")

            send({
                "protocol": "maelys-provider/3", "id": 2, "method": "provider/call",
                "params": {"name": "fixture.echo", "arguments": {"message": "over the socket"}},
            })
            call_response = json.loads(reader.readline())
            self.assertEqual(call_response["result"]["structuredContent"]["message"], "over the socket")

            send({"protocol": "maelys-provider/3", "id": 3, "method": "provider/shutdown", "params": {}})
            shutdown_response = json.loads(reader.readline())
            self.assertEqual(shutdown_response["id"], 3)

            stdout_bytes, stderr_bytes = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, stderr_bytes)
            # The whole session went over the adopted fd, never mixed into
            # stdout - that is what matters here. Where the handler's
            # print() landed is deliberately NOT asserted: under
            # MAELYS_PROVIDER_FD this SDK skips isolate_stdout entirely
            # (docs/launch-contract-design.md, "the SDK should ALSO leave
            # fd 1 pointed wherever the host wired it - no SDK-side
            # redirection needed"), so print() reaching real fd 1 is correct
            # SDK behaviour in this harness, which - unlike the real POSIX
            # launcher under MAELYS_MCP_PROCESS_FD_ISOLATED - does not
            # itself wire fd 1 to stderr before exec. That kernel-level
            # wiring is the launcher's job, proven separately by
            # tests/test_process_launcher.c's case_isolated_layout, not
            # something this SDK-only harness can or should reproduce.
            self.assertNotIn(b'"protocol"', stdout_bytes)
        finally:
            parent_sock.close()

    def test_absent_provider_fd_still_speaks_plain_stdio(self) -> None:
        env = _env_with_sdk_on_path()
        env.pop("MAELYS_PROVIDER_FD", None)
        process = subprocess.Popen(
            [sys.executable, FIXTURE],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env,
        )
        requests = [
            {"protocol": "maelys-provider/3", "id": 1, "method": "provider/describe", "params": {}},
            {"protocol": "maelys-provider/3", "id": 2, "method": "provider/shutdown", "params": {}},
        ]
        payload = "".join(json.dumps(request) + "\n" for request in requests).encode("utf-8")
        stdout_bytes, _ = process.communicate(payload, timeout=10)
        self.assertEqual(process.returncode, 0)
        lines = [line for line in stdout_bytes.decode("utf-8").split("\n") if line]
        messages = [json.loads(line) for line in lines]
        self.assertEqual([message["id"] for message in messages], [1, 2])


if __name__ == "__main__":
    unittest.main()
