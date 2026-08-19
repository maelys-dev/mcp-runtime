#!/usr/bin/env python3
"""Run the official MCP suite against the local stdio facade, for both eras.

The HTTP listener is a test adapter, not a supported runtime transport. It forwards
one JSON-RPC object per POST to the stdio host and returns its single response.

Two passes run, because the two eras have incompatible session models:

- 2026-07-28 (modern): stateless, no `initialize` handshake, every request is
  self-contained via per-request `_meta`. One runtime subprocess is shared across
  every scenario in MODERN_SCENARIOS — there is no session state to leak between
  scenario runs, so reuse is both safe and much cheaper.
- 2025-11-25 (legacy): stateful, one `initialize`/`notifications/initialized`
  handshake per session, exactly like a real legacy MCP client. Each scenario in
  LEGACY_SCENARIOS therefore gets its OWN fresh runtime subprocess (and thus its
  own channel) — reusing one process across scenarios makes the second scenario's
  `initialize` fail with "-32600 Initialize already received", since mcp-runtime
  correctly rejects re-initializing an already-initialized channel.

The legacy pass's scenario list is NOT hardcoded: `legacy_scenarios()` below
asks the official CLI itself (`list --server --requirements 2025-11-25`) for
the current frozen requirement set, then subtracts LEGACY_EXCLUDED. Only the
exclusions are hardcoded, keyed by scenario name with a reason each — a
scenario the official CLI adds, renames or drops is picked up (or silently
stops applying) automatically; only mcp-runtime's OWN scope boundary needs a
human to update it, and even that half is caught by two live checks rather
than left to rot as a comment:

- `check_exclusions_still_hold()` sends each "not implemented at all" method
  (see LEGACY_EXCLUDED) over a real legacy session and fails loudly if any of
  them stops returning -32601 Method not found — i.e. if mcp-runtime grew that
  feature and nobody updated LEGACY_EXCLUDED.
- `legacy_scenarios()` itself warns if an entry in LEGACY_EXCLUDED no longer
  appears in the upstream list at all (renamed or removed upstream) — a dead
  exclusion is otherwise invisible since removing it changes nothing.

The "architectural mismatch" and "transport-specific" reasons in
LEGACY_EXCLUDED (see the dict below) can't be checked live the same way —
they're about *how* a supported method is used, not *whether* the method
exists — so they stay as comments backed by the mcp-runtime source facts and
the official scenario source (`src/scenarios/server/tools.ts` upstream) they
were verified against.

MODERN_SCENARIOS, by contrast, is still hardcoded and does NOT cover the full
2026-07-28 requirement set (36 required, 14 run here) - the same
dynamic-list-minus-exclusions treatment applies there too in principle, since
the draft spec changes more often than a frozen one, but working out which of
the missing `input-required-result-*` depth scenarios are genuinely
achievable with the current mock provider is a separate, larger piece of work
than what this pass covers today.
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


MODERN_SCENARIOS = (
    "tools-list",
    "tools-call-simple-text",
    "tools-call-image",
    "tools-call-audio",
    "tools-call-embedded-resource",
    "tools-call-mixed-content",
    "tools-call-error",
    "tools-call-with-progress",
    "resources-list",
    "resources-read-text",
    "resources-read-binary",
    "resources-templates-read",
    "sep-2164-resource-not-found",
    "input-required-result-basic-elicitation",
    "input-required-result-capability-check",
)

# Scenarios required for 2025-11-25 that mcp-runtime cannot meaningfully run,
# keyed by name with a one-line reason. See legacy_scenarios() for how this
# combines with the live, upstream-sourced requirement list.
LEGACY_EXCLUDED = {
    # Feature not implemented by mcp-runtime at all - it has exactly four
    # modules (tools, MRTR, resources, subscriptions); none of these exist.
    "logging-set-level": "no logging module",
    "ping": "no ping handler",
    "completion-complete": "no completion module",
    "tools-call-with-logging": "no logging notifications",
    "prompts-list": "no prompts module",
    "prompts-get-simple": "no prompts module",
    "prompts-get-with-args": "no prompts module",
    "prompts-get-embedded-resource": "no prompts module",
    "prompts-get-with-image": "no prompts module",
    # Architectural mismatch: these require the server to send a NESTED
    # sampling/createMessage or elicitation/create request mid-tools/call,
    # synchronously, over the same connection, blocking for the client's
    # reply before completing the original call (verified against the
    # official scenario source, src/scenarios/server/tools.ts and
    # elicitation-{defaults,enums}.ts). mcp-runtime's MRTR module implements
    # only the resumable pattern (return an input_required result; the caller
    # retries tools/call with inputResponses) - the same pattern the
    # 2026-07-28 input-required-result-* scenarios test, which this script
    # already runs. That's also why 2026-07-28 dropped the nested variant:
    # no tools-call-sampling or tools-call-elicitation scenario exists in the
    # 2026-07-28 requirement set at all.
    #
    # Caveat this exclusion surfaced: InputRequiredResult (resultType,
    # inputRequests, requestState) is itself a 2026-07-28-only schema type -
    # the official 2025-06-18/2025-11-25 CallToolResult schema has no
    # resultType field and requires `content` unconditionally. So today's
    # legacy input_required support (see docs/architecture.md) is understood
    # by mcp-runtime and by clients that were specifically updated for it
    # (e.g. Hermes) - not by a generic, schema-validating 2025-11-25 client,
    # which would see a response missing its required `content` field. That's
    # a real gap in spec fidelity, not just a missing test scenario; closing
    # it needs an explicit decision on how far to take it (see the PR/commit
    # this constant landed in for that discussion), not a silent scenario
    # addition here.
    "tools-call-sampling": "requires the nested in-band pattern; see the block comment above",
    "tools-call-elicitation": "requires the nested in-band pattern; see the block comment above",
    "elicitation-sep1034-defaults": "same nested pattern as tools-call-elicitation",
    "elicitation-sep1330-enums": "same nested pattern as tools-call-elicitation",
    # Standard method not implemented: mcp-runtime's resources module handles
    # exactly resources/list, resources/templates/list, resources/read (see
    # handles() in src/modules/resources.c). Its "subscriptions" module
    # implements subscriptions/listen instead - the real 2026-07-28 primitive
    # (see docs/subscriptions.md), not a maelys invention - but that module
    # currently rejects legacy channels outright ("Subscriptions require MCP
    # 2026-07-28", src/modules/subscriptions.c) the same way tools.c and
    # resources.c did before this fix landed. Extending it to legacy channels
    # that declare the right capability is the natural next step in this same
    # pattern; the legacy-only resources/subscribe and resources/unsubscribe
    # methods this scenario pair specifically tests are not planned.
    "resources-subscribe": "mcp-runtime exposes subscriptions/listen, not the legacy resources/subscribe method",
    "resources-unsubscribe": "mcp-runtime exposes subscriptions/listen, not the legacy resources/unsubscribe method",
    # Transport-specific, N/A to this test adapter: this bridge is a
    # single-response-per-POST test adapter (see module docstring), not an
    # implementation of the Streamable HTTP transport's SSE semantics, nor of
    # the Host/Origin header validation a real network-facing HTTP listener
    # needs (this one only ever binds 127.0.0.1 for the duration of one
    # scenario run - it is not a production transport, see the module
    # docstring). Confirmed live: dns-rebinding-protection fails both its
    # checks against this bridge for exactly that reason (ECONNREFUSED /
    # missing Host-header rejection), not because of anything mcp-runtime did.
    "server-sse-multiple-streams": "this bridge does not implement SSE transport semantics",
    "dns-rebinding-protection": "this bridge does not implement Host/Origin header validation",
}

MAX_BODY_BYTES = 4 * 1024 * 1024


def legacy_scenarios(package: str) -> list[str]:
    """The 2025-11-25 server requirement set, fetched live from the official
    tool and filtered by LEGACY_EXCLUDED - not a hand-copied list that can
    silently drift out of sync with what upstream actually requires."""
    completed = subprocess.run(
        ["npx", "-y", package, "list", "--server", "--requirements", "2025-11-25"],
        check=True, capture_output=True, text=True,
    )
    lines = completed.stdout.splitlines()
    try:
        start = lines.index("Server scenarios (test against a server):") + 1
    except ValueError:
        raise RuntimeError(
            "could not find 'Server scenarios (test against a server):' in "
            "`list --server --requirements 2025-11-25` output - the official "
            "CLI's output format may have changed"
        )
    all_scenarios = []
    for line in lines[start:]:
        if not line.startswith("  - ") or line.startswith("    "):
            break
        all_scenarios.append(line.removeprefix("  - "))
    if not all_scenarios:
        raise RuntimeError(
            "parsed zero server scenarios for the 2025-11-25 requirement set - "
            "the official CLI's output format may have changed"
        )
    stale = sorted(set(LEGACY_EXCLUDED) - set(all_scenarios))
    if stale:
        print(
            "warning: LEGACY_EXCLUDED has entries no longer in the upstream "
            f"2025-11-25 requirement list (renamed or removed?): {stale}",
            file=sys.stderr,
        )
    return [name for name in all_scenarios if name not in LEGACY_EXCLUDED]


def _is_notification(body: bytes) -> bool:
    """A JSON-RPC message with no `id` is a notification: mcp-runtime writes no
    response line for it. A bridge that unconditionally blocks on readline()
    after forwarding one hangs forever - this is what makes the legacy pass
    different from the modern one, since the stateless modern era never sends
    `notifications/initialized` (no session, no handshake) while every legacy
    session does."""
    try:
        parsed = json.loads(body)
    except (ValueError, json.JSONDecodeError):
        return False
    return isinstance(parsed, dict) and "id" not in parsed


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

    def request(self, body: bytes) -> list[bytes] | None:
        """Returns every line the runtime emits for this request, the final
        response last.

        A request can produce request-scoped notifications - progress - ahead
        of its response. Reading exactly one line would hand the caller the
        first notification as if it were the response, and leave the rest to
        corrupt the next request; that is precisely what happened when
        tools-call-with-progress was first enabled. Lines are therefore read
        until the one carrying this request's id arrives.
        """
        with self._lock:
            if self._process.poll() is not None:
                raise RuntimeError(f"runtime exited with status {self._process.returncode}")
            frame = body.rstrip(b"\r\n") + b"\n"
            self._input.write(frame)
            self._input.flush()
            if _is_notification(body):
                return None
            wanted = json.loads(body).get("id")
            lines: list[bytes] = []
            while True:
                line = self._output.readline(MAX_BODY_BYTES + 1)
                if not line:
                    raise RuntimeError("runtime closed stdout without a response")
                if len(line) > MAX_BODY_BYTES:
                    raise RuntimeError("runtime response exceeds the bridge limit")
                line = line.rstrip(b"\r\n")
                lines.append(line)
                try:
                    parsed = json.loads(line)
                except json.JSONDecodeError:
                    raise RuntimeError("runtime emitted a line that is not JSON")
                if isinstance(parsed, dict) and parsed.get("id") == wanted:
                    return lines

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
                lines = bridge.request(body)
            except (ValueError, json.JSONDecodeError) as error:
                payload = json.dumps({"error": str(error)}).encode()
                self._send(400, payload)
                return
            except Exception as error:  # keep diagnostics at the test boundary
                payload = json.dumps({"error": str(error)}).encode()
                self._send(502, payload)
                return
            if lines is None:
                # Notification: no JSON-RPC reply, per the Streamable HTTP spec.
                self._send(202, b"")
                return
            if len(lines) == 1:
                self._send(200, lines[0])
                return
            # Notifications preceded the response, so the spec requires this
            # exchange to be an SSE stream rather than a single JSON object.
            payload = b"".join(b"data: " + line + b"\r\n\r\n" for line in lines)
            self._send(200, payload, content_type="text/event-stream")

        def _send(self, status: int, payload: bytes,
                  content_type: str = "application/json") -> None:
            self.send_response(status)
            if payload:
                self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            if payload:
                self.wfile.write(payload)

        def log_message(self, _format: str, *_arguments: object) -> None:
            return

    return Handler


def _serve(bridge: StdioBridge) -> tuple[ThreadingHTTPServer, threading.Thread, str]:
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler_for(bridge))
    thread = threading.Thread(target=server.serve_forever, name="mcp-test-http", daemon=True)
    thread.start()
    url = f"http://127.0.0.1:{server.server_port}/mcp"
    return server, thread, url


def _teardown(server: ThreadingHTTPServer, thread: threading.Thread, bridge: StdioBridge) -> None:
    server.shutdown()
    server.server_close()
    thread.join(timeout=5)
    bridge.close()


def _run_scenario(
    package: str, url: str, scenario: str, spec_version: str, output_dir: Path
) -> bool:
    print(f"official MCP conformance ({spec_version}): {scenario}", flush=True)
    command = [
        "npx", "-y", package, "server",
        "--url", url,
        "--scenario", scenario,
        "--spec-version", spec_version,
        "--output-dir", str(output_dir / scenario),
    ]
    completed = subprocess.run(command, check=False)
    return completed.returncode == 0


def run_modern_pass(runtime: Path, provider: Path, package: str, output_root: Path) -> list[str]:
    """One shared runtime subprocess for the whole pass: 2026-07-28 is stateless,
    so there is no session state that could leak between scenario runs."""
    bridge = StdioBridge([str(runtime), "--provider", str(provider)])
    server, thread, url = _serve(bridge)
    failures: list[str] = []
    try:
        for scenario in MODERN_SCENARIOS:
            if not _run_scenario(package, url, scenario, "2026-07-28", output_root):
                failures.append(f"2026-07-28/{scenario}")
    finally:
        _teardown(server, thread, bridge)
    return failures


def run_legacy_pass(runtime: Path, provider: Path, package: str, output_root: Path) -> list[str]:
    """A fresh runtime subprocess per scenario: 2025-11-25 is a stateful session,
    and each scenario run is an independent client connecting fresh. Reusing one
    process across scenarios would make every scenario after the first fail its
    own `initialize` with "-32600 Initialize already received"."""
    failures: list[str] = []
    for scenario in legacy_scenarios(package):
        bridge = StdioBridge([str(runtime), "--provider", str(provider)])
        server, thread, url = _serve(bridge)
        try:
            if not _run_scenario(package, url, scenario, "2025-11-25", output_root):
                failures.append(f"2025-11-25/{scenario}")
        finally:
            _teardown(server, thread, bridge)
    return failures


# Methods behind every scenario this script excludes as "not implemented at
# all" (as opposed to "wrong pattern" or "wrong transport", which need a real
# protocol exchange to prove). If mcp-runtime ever implements one of these,
# this check starts failing loudly instead of the exclusion silently going
# stale - remove the corresponding entry from LEGACY_EXCLUDED (or add the
# scenario to MODERN_SCENARIOS) when it does.
UNIMPLEMENTED_METHODS = (
    "resources/subscribe",
    "resources/unsubscribe",
    "prompts/list",
    "logging/setLevel",
    "ping",
    "completion/complete",
)


def check_exclusions_still_hold(runtime: Path, provider: Path) -> list[str]:
    bridge = StdioBridge([str(runtime), "--provider", str(provider)])
    stale: list[str] = []
    try:
        init = json.dumps({
            "jsonrpc": "2.0", "id": 1, "method": "initialize",
            "params": {
                "protocolVersion": "2025-11-25", "capabilities": {},
                "clientInfo": {"name": "scope-canary", "version": "1"},
            },
        }).encode()
        bridge.request(init)
        bridge.request(json.dumps({
            "jsonrpc": "2.0", "method": "notifications/initialized", "params": {},
        }).encode())
        for index, method in enumerate(UNIMPLEMENTED_METHODS, start=2):
            request = json.dumps({
                "jsonrpc": "2.0", "id": index, "method": method, "params": {},
            }).encode()
            # request() returns every line for the exchange, the response
            # last; a probe never produces notifications, but read it that way
            # regardless rather than assuming.
            lines = bridge.request(request)
            envelope = json.loads(lines[-1]) if lines else {}
            code = envelope.get("error", {}).get("code")
            if code != -32601:
                stale.append(
                    f"{method} no longer returns -32601 Method not found "
                    f"(got {envelope!r}) - it looks implemented now; move its "
                    f"scenario out of the exclusion list in this file's docstring"
                )
    finally:
        bridge.close()
    return stale


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

    stale_exclusions = check_exclusions_still_hold(runtime, provider)
    if stale_exclusions:
        print("official MCP conformance exclusions are stale:", file=sys.stderr)
        for line in stale_exclusions:
            print(f"  - {line}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="mcp-conformance-") as output_root:
        root = Path(output_root)
        failures = run_modern_pass(runtime, provider, args.package, root)
        failures += run_legacy_pass(runtime, provider, args.package, root)

    if failures:
        print(f"official MCP scenarios failed: {', '.join(failures)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
