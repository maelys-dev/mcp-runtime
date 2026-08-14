#!/usr/bin/env python3
"""Run the official HTTP-only MCP suite against the local stdio facade.

The HTTP listener is a test adapter, not a supported runtime transport. It forwards
one JSON-RPC object per POST to the stdio host and returns its single response.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import BinaryIO


SCENARIOS = (
    "tools-list",
    "tools-call-simple-text",
    "tools-call-image",
    "tools-call-audio",
    "tools-call-embedded-resource",
    "tools-call-mixed-content",
    "tools-call-error",
    "input-required-result-basic-elicitation",
    "input-required-result-capability-check",
)
MAX_BODY_BYTES = 4 * 1024 * 1024


class StdioBridge:
    def __init__(self, command: list[str]) -> None:
        self._process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=None,
        )
        if self._process.stdin is None or self._process.stdout is None:
            self.close()
            raise RuntimeError("failed to open runtime stdio")
        self._input: BinaryIO = self._process.stdin
        self._output: BinaryIO = self._process.stdout
        self._lock = threading.Lock()

    def request(self, body: bytes) -> bytes:
        with self._lock:
            if self._process.poll() is not None:
                raise RuntimeError(f"runtime exited with status {self._process.returncode}")
            self._input.write(body.rstrip(b"\r\n") + b"\n")
            self._input.flush()
            response = self._output.readline(MAX_BODY_BYTES + 1)
            if not response:
                raise RuntimeError("runtime closed stdout without a response")
            if len(response) > MAX_BODY_BYTES:
                raise RuntimeError("runtime response exceeds the bridge limit")
            return response.rstrip(b"\r\n")

    def close(self) -> None:
        process = getattr(self, "_process", None)
        if process is None:
            return
        if process.stdin is not None and not process.stdin.closed:
            process.stdin.close()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)


def handler_for(bridge: StdioBridge) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
            if self.path != "/mcp":
                self.send_error(404)
                return
            try:
                length = int(self.headers.get("Content-Length", ""))
                if length < 1 or length > MAX_BODY_BYTES:
                    raise ValueError("invalid request size")
                body = self.rfile.read(length)
                parsed = json.loads(body)
                if not isinstance(parsed, dict):
                    raise ValueError("request body must be a JSON object")
                response = bridge.request(body)
                envelope = json.loads(response)
                if not isinstance(envelope, dict):
                    raise ValueError("runtime response must be a JSON object")
            except (ValueError, json.JSONDecodeError) as error:
                payload = json.dumps({"error": str(error)}).encode()
                self._send(400, payload)
                return
            except Exception as error:  # keep diagnostics at the test boundary
                payload = json.dumps({"error": str(error)}).encode()
                self._send(502, payload)
                return
            self._send(200, response)

        def _send(self, status: int, payload: bytes) -> None:
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def log_message(self, _format: str, *_arguments: object) -> None:
            return

    return Handler


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--provider", type=Path, required=True)
    parser.add_argument("--package", required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    runtime = args.runtime.resolve()
    provider = args.provider.resolve()
    if not runtime.is_file() or not os.access(runtime, os.X_OK):
        raise SystemExit(f"runtime is not executable: {runtime}")
    if not provider.is_file():
        raise SystemExit(f"provider does not exist: {provider}")

    bridge = StdioBridge([str(runtime), "--provider", str(provider)])
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler_for(bridge))
    thread = threading.Thread(target=server.serve_forever, name="mcp-test-http", daemon=True)
    thread.start()
    url = f"http://127.0.0.1:{server.server_port}/mcp"
    failures: list[str] = []
    try:
        with tempfile.TemporaryDirectory(prefix="mcp-conformance-") as output_root:
            for scenario in SCENARIOS:
                print(f"official MCP conformance: {scenario}", flush=True)
                command = [
                    "npx", "-y", args.package, "server",
                    "--url", url,
                    "--scenario", scenario,
                    "--spec-version", "2026-07-28",
                    "--output-dir", str(Path(output_root) / scenario),
                ]
                completed = subprocess.run(command, check=False)
                if completed.returncode != 0:
                    failures.append(scenario)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
        bridge.close()
    if failures:
        print(f"official MCP scenarios failed: {', '.join(failures)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
