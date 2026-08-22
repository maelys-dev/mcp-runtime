#!/usr/bin/env python3
"""Run the official MCP suite against mcp-runtime, for both eras - and, since
H4, over two DIFFERENT transports, on purpose:

- 2026-07-28 (modern) runs directly against the real HTTP listener
  (`--http-listen`), the actual Streamable HTTP transport this binary ships.
  No adapter sits in the path: the official runner's HTTP client talks
  straight to `maelys-mcp`'s socket. One runtime subprocess is shared across
  every scenario in MODERN_SCENARIOS - 2026-07-28 is stateless, no
  `initialize` handshake, every request is self-contained via per-request
  `_meta` - so there is no session state that could leak between scenario
  runs, and reuse is both safe and much cheaper.
- 2025-11-25 (legacy) still runs over `StdioBridge`, a test adapter that
  forwards one JSON-RPC object per POST to a stdio-hosted runtime and returns
  its response. This is not a leftover: legacy stays stdio-only BY DESIGN
  (`maelys-mcp --help` - the HTTP listener serves 2026-07-28 ONLY), so a
  bridge is the only way to point the official runner's HTTP-only client at
  it at all. The two passes are therefore asymmetric on purpose, not by
  omission: modern dropped the bridge because it finally has a real HTTP
  transport to test; legacy keeps it because it has no HTTP transport to
  test. Each scenario in LEGACY_SCENARIOS gets its OWN fresh runtime
  subprocess (and thus its own channel) - reusing one process across
  scenarios makes the second scenario's `initialize` fail with "-32600
  Initialize already received", since mcp-runtime correctly rejects
  re-initializing an already-initialized channel.

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

The "architectural mismatch" reason in LEGACY_EXCLUDED (see the dict below)
can't be checked live the same way — it's about *how* a supported method is
used, not *whether* the method exists — so it stays as a comment backed by
the mcp-runtime source facts and the official scenario source
(`src/scenarios/server/tools.ts` upstream) it was verified against.

`server-sse-multiple-streams` and `dns-rebinding-protection` used to be
excluded from BOTH passes as "transport-specific, N/A to this test adapter":
the bridge implemented neither SSE transport semantics nor Host/Origin header
validation. That reasoning is now stale for the modern pass specifically,
because the modern pass no longer has a bridge in the path at all - the real
HTTP listener implements both (`docs/http-transport-design.md`'s "SSE
framing" and "Inbound headers" sections), so both scenarios now run directly
against it in MODERN_SCENARIOS. They remain excluded from LEGACY_EXCLUDED,
unchanged, because the legacy pass still goes over the bridge, which still
implements neither.

MODERN_SCENARIOS is still hardcoded and does NOT cover the full 2026-07-28
requirement set (69 required, 17 run here) - the same dynamic-list-minus-
exclusions treatment applies there too in principle, since the draft spec
changes more often than a frozen one, but working out which of the missing
`input-required-result-*` depth scenarios are genuinely achievable with the
current mock provider is a separate, larger piece of work than what this pass
covers today.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
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
    # H4: runs directly against the real HTTP listener's chunked SSE framing -
    # the bridge could not implement this (see LEGACY_EXCLUDED and the module
    # docstring), which is why it stayed out of this tuple until now.
    "server-sse-multiple-streams",
    "resources-list",
    "resources-read-text",
    "resources-read-binary",
    "resources-templates-read",
    "sep-2164-resource-not-found",
    # H4: runs directly against the real HTTP listener's Host/Origin
    # validation - same story as server-sse-multiple-streams above.
    "dns-rebinding-protection",
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
    "completion-complete": "no completion module",
    "tools-call-with-logging": "no logging notifications",
    "prompts-list": "no prompts module",
    "prompts-get-simple": "no prompts module",
    "prompts-get-with-args": "no prompts module",
    "prompts-get-embedded-resource": "no prompts module",
    "prompts-get-with-image": "no prompts module",
    # tools-call-sampling, tools-call-elicitation, elicitation-sep1034-defaults
    # and elicitation-sep1330-enums used to be excluded here: they require the
    # server to send a NESTED sampling/createMessage or elicitation/create
    # request mid-tools/call, synchronously over the same connection, blocking
    # for the client's reply before completing the original call (verified
    # against the official scenario source, src/scenarios/server/tools.ts and
    # elicitation-{defaults,enums}.ts) - a second MRTR shape mcp-runtime's host
    # core, all three provider SDKs and this fixture provider now implement
    # alongside the resumable one (return an input_required result; the caller
    # retries tools/call with inputResponses), which is what the 2026-07-28
    # input-required-result-* scenarios this script already runs exercise
    # instead. See docs/architecture.md's MRTR section for both shapes, and
    # conformance/official_tools_provider.py's test_sampling/test_elicitation/
    # test_elicitation_sep1034_defaults/test_elicitation_sep1330_enums
    # handlers (built on the Python SDK's context.request_sampling /
    # context.request_elicitation) for this fixture's side of it. That is
    # also still why 2026-07-28 has no nested variant of its own: no
    # tools-call-sampling or tools-call-elicitation scenario exists in the
    # 2026-07-28 requirement set at all, only the resumable one.
    #
    # The InputRequiredResult (resultType, inputRequests, requestState) caveat
    # the old exclusion recorded still stands and is unrelated to this: it is
    # itself a 2026-07-28-only schema type - the official 2025-06-18/2025-11-25
    # CallToolResult schema has no resultType field and requires `content`
    # unconditionally - so legacy input_required support (see
    # docs/architecture.md) remains understood only by mcp-runtime and clients
    # specifically updated for it (e.g. Hermes), not by a generic,
    # schema-validating 2025-11-25 client. The nested shape closes exactly the
    # four scenarios above; it does not change that caveat.
    #
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
    # Transport-specific, N/A to THIS PASS'S test adapter: StdioBridge is a
    # single-response-per-POST test adapter (see module docstring), not an
    # implementation of the Streamable HTTP transport's SSE semantics, nor of
    # the Host/Origin header validation a real network-facing HTTP listener
    # needs (it only ever binds 127.0.0.1 for the duration of one scenario
    # run - it is not a production transport, see the module docstring).
    # Confirmed live: dns-rebinding-protection fails both its checks against
    # this bridge for exactly that reason (ECONNREFUSED / missing Host-header
    # rejection), not because of anything mcp-runtime did.
    #
    # This exclusion is legacy-only as of H4. Both scenarios now run in
    # MODERN_SCENARIOS instead, directly against the real HTTP listener,
    # which does implement SSE framing and Host/Origin validation - that
    # listener serves 2026-07-28 ONLY (`maelys-mcp --help`), so the legacy
    # pass has no such listener to point at and keeps using this bridge.
    "server-sse-multiple-streams": "this bridge does not implement SSE transport semantics (legacy-only exclusion - see MODERN_SCENARIOS)",
    "dns-rebinding-protection": "this bridge does not implement Host/Origin header validation (legacy-only exclusion - see MODERN_SCENARIOS)",
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


def _expects_reply(body: bytes) -> bool:
    """True only for a JSON-RPC REQUEST - has both `method` and `id`.

    A notification (no `id`) gets no response line from mcp-runtime, ever.
    Neither does a plain JSON-RPC response (has `id`, no `method`) - the shape
    a real MCP client's reply to a server-initiated NESTED request takes
    (`elicitation/create`/`sampling/createMessage` mid-`tools/call`; see
    docs/architecture.md "Nested requests"). Both are forward-and-forget: a
    bridge that unconditionally blocked on readline() after forwarding either
    one would hang forever, waiting for "a response to a response" that the
    runtime never sends - a reply is answered by the ORIGINAL call's own
    still-open stream resuming, not by a response of its own.
    """
    try:
        parsed = json.loads(body)
    except (ValueError, json.JSONDecodeError):
        return False
    return isinstance(parsed, dict) and "id" in parsed and "method" in parsed


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
        # Two locks, not one: writing a reply to an outstanding nested
        # request (`send`, below) must never wait behind a call that is still
        # mid-`stream()` - that was exactly the deadlock this replaced (see
        # the commit this landed in). `_write_lock` alone serializes writes;
        # `_stream_lock` additionally serializes one call's whole read
        # lifecycle, since only one call is ever "the current streaming
        # exchange" in these scenarios (the client never pipelines a second
        # tools/call before the first completes).
        self._write_lock = threading.Lock()
        self._stream_lock = threading.Lock()

    def _write(self, body: bytes) -> None:
        if self._process.poll() is not None:
            raise RuntimeError(f"runtime exited with status {self._process.returncode}")
        frame = body.rstrip(b"\r\n") + b"\n"
        with self._write_lock:
            self._input.write(frame)
            self._input.flush()

    def send(self, body: bytes) -> None:
        """Forwards a notification or a nested-request reply and returns
        immediately - see _expects_reply for why neither waits for output."""
        self._write(body)

    def stream(self, body: bytes):
        """Writes a JSON-RPC request and yields `(line, is_final)` for every
        line the runtime emits for it, one at a time as it arrives - the line
        carrying this request's own id last, with `is_final` True only on it.

        Progressive, not buffered-then-returned: a request can produce
        request-scoped notifications - progress - or, since nested MRTR
        landed, a server-initiated request (`elicitation/create`,
        `sampling/createMessage`) ahead of its own response. A caller that
        only sees the whole batch once every line has already arrived can
        never forward an intervening line to the real HTTP client while this
        call's connection is still open - and a client that is never shown a
        nested request it must answer has nothing to reply to, which is what
        produced "-32001: Request timed out" on tools-call-sampling/
        tools-call-elicitation before this generator existed. Reading exactly
        one line and stopping has the mirror-image bug (tools-call-with-progress
        first caught it): the first notification would be handed back as if
        it were the response, corrupting whatever reads the connection next.
        """
        wanted = json.loads(body).get("id")
        with self._stream_lock:
            self._write(body)
            while True:
                line = self._output.readline(MAX_BODY_BYTES + 1)
                if not line:
                    raise RuntimeError("runtime closed stdout without a response")
                if len(line) > MAX_BODY_BYTES:
                    raise RuntimeError("runtime response exceeds the bridge limit")
                line = line.rstrip(b"\r\n")
                try:
                    parsed = json.loads(line)
                except json.JSONDecodeError:
                    raise RuntimeError("runtime emitted a line that is not JSON")
                is_final = isinstance(parsed, dict) and parsed.get("id") == wanted
                yield line, is_final
                if is_final:
                    return

    def request(self, body: bytes) -> list[bytes] | None:
        """The fully-buffered convenience `check_exclusions_still_hold` uses:
        every line for one simple, non-nesting exchange, response last. The
        HTTP-facing handler below does not use this - it needs `stream`'s
        progressive delivery, not a batch collected after the fact.
        """
        if not _expects_reply(body):
            self.send(body)
            return None
        return [line for line, _ in self.stream(body)]

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
    # Legacy-pass-only as of H4 (see the module docstring): the modern pass
    # no longer wraps a bridge in an HTTP handler at all - HttpDirect points
    # the official runner straight at the real listener instead.
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
            except (ValueError, json.JSONDecodeError) as error:
                payload = json.dumps({"error": str(error)}).encode()
                self._send(400, payload)
                return
            if not _expects_reply(body):
                # Notification, or a client's reply to a server-initiated
                # nested request: forward-and-forget either way (see
                # _expects_reply) - no JSON-RPC reply is ever generated, per
                # the Streamable HTTP spec.
                try:
                    bridge.send(body)
                except Exception as error:  # keep diagnostics at the test boundary
                    payload = json.dumps({"error": str(error)}).encode()
                    self._send(502, payload)
                    return
                self._send(202, b"")
                return
            try:
                self._relay(bridge.stream(body))
            except Exception as error:  # keep diagnostics at the test boundary
                payload = json.dumps({"error": str(error)}).encode()
                self._send(502, payload)

        def _relay(self, lines) -> None:
            """Sends the first line as a plain JSON response if it is already
            the final one - unchanged from before streaming existed, for
            every scenario that never nests or reports progress - or switches
            to a chunked SSE stream the moment a line turns out not to be
            final, flushing each line to the wire as it arrives rather than
            once the whole exchange is done. See StdioBridge.stream for why
            that distinction matters for the nested pattern.
            """
            iterator = iter(lines)
            try:
                line, is_final = next(iterator)
            except StopIteration:
                raise RuntimeError("runtime produced no output for a request expecting one")
            if is_final:
                self._send(200, line)
                return
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self._write_chunk(line)
            for line, _ in iterator:
                self._write_chunk(line)
            self._write_chunk(b"")  # zero-length chunk: end of body, per RFC 9112

        def _write_chunk(self, line: bytes) -> None:
            payload = b"data: " + line + b"\r\n\r\n" if line else b""
            self.wfile.write(f"{len(payload):x}\r\n".encode() + payload + b"\r\n")
            self.wfile.flush()

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


def _find_free_port() -> int:
    """Picks a currently-unused loopback port for `--http-listen` to bind.

    There is a TOCTOU race between closing this probe socket and the runtime
    binary binding the same port a moment later. That is an accepted
    trade-off, not an oversight: `maelys-mcp` has no "bind port 0, tell me
    what you got" mode to remove the race outright (`--http-listen` takes a
    literal `ADDRESS:PORT`, see `host/main.c`'s `parse_listen`), and a
    collision on this single-process, single-run test harness fails loudly -
    `HttpDirect._wait_ready` below turns "the runtime never came up" into a
    clear `RuntimeError`/`TimeoutError` rather than a silent false pass.
    """
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]
    finally:
        probe.close()


class HttpDirect:
    """Runs the real `maelys-mcp` binary with its HTTP listener enabled and
    nothing else in front of it - the H4 replacement for `StdioBridge` on the
    modern pass. `maelys-mcp` serves stdio alongside HTTP unconditionally
    (there is no flag to turn stdio off) and exits the moment its stdin
    reaches EOF, so this class holds `stdin` open on a pipe it never writes
    to, for the same reason `StdioBridge` holds a write lock across its own
    process's lifetime: closing that pipe is the deliberate shutdown signal
    (see `close()`), not an accident of `Popen`'s defaults.

    `--http-allow-origin` is set to this run's own URL, confirmed live to be
    exactly the Origin the official runner's HTTP client sends (see H4's red
    evidence in the PR description) - `dns-rebinding-protection` fails with
    403 without it, because a loopback bind still requires an explicit
    allowlist entry for any Origin it actually receives (only a REQUEST WITH
    NO Origin header is accepted on loopback by default -
    `maelys_http_origin_allowed`, `host/http_parser.c`). This narrows nothing
    the shipped binary defaults to: the allowlist is still empty unless a
    caller of `maelys-mcp` opts in, exactly as it does in production.
    """

    def __init__(self, runtime: Path, provider: Path) -> None:
        self.port = _find_free_port()
        self.origin = f"http://127.0.0.1:{self.port}"
        self.url = f"{self.origin}/mcp"
        self._process = subprocess.Popen(
            [
                str(runtime), "--provider", str(provider),
                "--http-listen", f"127.0.0.1:{self.port}",
                "--http-path", "/mcp",
                "--http-allow-origin", self.origin,
            ],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        self._wait_ready()

    def _wait_ready(self, timeout: float = 10.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self._process.poll() is not None:
                stderr = (self._process.stderr.read().decode(errors="replace")
                    if self._process.stderr else "")
                raise RuntimeError(
                    "maelys-mcp exited before its HTTP listener came up "
                    f"(status {self._process.returncode}): {stderr}"
                )
            probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                if probe.connect_ex(("127.0.0.1", self.port)) == 0:
                    return
            finally:
                probe.close()
            time.sleep(0.05)
        raise TimeoutError(
            f"maelys-mcp's HTTP listener never accepted a connection on port {self.port}"
        )

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
    """One shared runtime subprocess for the whole pass, its real HTTP
    listener driven directly with no adapter in the path: 2026-07-28 is
    stateless, so there is no session state that could leak between scenario
    runs."""
    direct = HttpDirect(runtime, provider)
    failures: list[str] = []
    try:
        for scenario in MODERN_SCENARIOS:
            if not _run_scenario(package, direct.url, scenario, "2026-07-28", output_root):
                failures.append(f"2026-07-28/{scenario}")
    finally:
        direct.close()
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
