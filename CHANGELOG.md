# Changelog

## 0.12.1 - 2026-08-17

- Fix a regression where any request carrying an opaque `_meta` object (for example
  a progress token) on an already legacy-initialized channel was incorrectly forced
  through modern `protocolVersion`/`clientCapabilities` validation and rejected with
  `-32602`. Only `_meta` that actually carries the
  `io.modelcontextprotocol/protocolVersion` key is now treated as a negotiation
  attempt — including on a legacy-initialized channel, where it is correctly
  rejected as an unsupported mid-session renegotiation. Fixes a startup failure
  observed with legacy MCP clients (e.g. Hermes) sending per-request metadata.

## 0.12.0 - 2026-08-17

- Negotiate the legacy `initialize` handshake instead of requiring an exact match on
  `2025-11-25`. The runtime now accepts `2024-11-05`, `2025-03-26`, `2025-06-18` and
  `2025-11-25`, and echoes the client's requested version back in the result, so
  clients (such as Codex) that announce an older dated revision are no longer rejected
  with `-32602`. `2026-07-28` support (stateless, via per-request `_meta`) is unchanged.

## 0.11.0 - 2026-08-17

- Add a tag-triggered binary release pipeline (`.github/workflows/release.yml`)
  with separated build and publish roles: the privileged publish job runs no
  candidate code, only verifies checksums and attaches artifacts.
- Ship per-platform static and dynamic tarballs (Linux x86_64/arm64, macOS
  arm64/x86_64) via `scripts/package-release.sh`, each with a SHA-256 checksum
  and a build provenance attestation.
- Move `VERSION` into a data file read by both `make` and the release workflow.

## 0.10.0 - 2026-08-15

- Introduce opaque `maelys_mcp_channel_t` handles. Each channel owns its ordered
  output queue, subscriptions, cancellation scope and legacy client lifecycle.
- Replace the runtime-wide writer outbox with passive bounded per-channel queues;
  transports now pump output through deadline-aware `maelys_mcp_channel_next`.
- Route every response, protocol error, listen acknowledgement, provider event and
  graceful completion through the channel queue. A response admission timeout faults
  only that channel; notification coalescence and rejection remain local.
- Make provider event fan-out multi-channel with stable per-channel references and
  local close barriers. Identical JSON-RPC subscription ids on different channels no
  longer collide or permit cross-delivery.
- Make provider activation runtime-scoped, once-only and fail-closed. Make
  `maelys_mcp_runtime_destroy` return `MAELYS_MCP_ERR_STATE` while a channel handle
  remains alive, including a closed handle not yet destroyed.
- Remove `maelys_mcp_runtime_handle`, `maelys_mcp_runtime_attach_outbox` and
  `maelys_mcp_runtime_detach_outbox` without compatibility wrappers. Migrate by
  creating a channel, passing requests to `maelys_mcp_channel_handle`, pumping output
  with `maelys_mcp_channel_next`, then closing and destroying the channel before the
  runtime.
- Increment the native ABI from 1 to 2. The `maelys-provider/3` wire protocol and C,
  TypeScript and Python provider SDK contracts are unchanged.

## 0.9.0 - 2026-08-15

- Bound stdio message writes with a configurable monotonic deadline and wake the
  service loop when the writer fails, preventing a non-draining client from freezing
  the runtime and its producers indefinitely.
- Add the backward-compatible `maelys_mcp_runtime_serve_stdio_with_options` API and
  `--stdio-write-timeout-ms` host option; the existing API uses a five-second default.
- Eliminate an unlocked `writer_status` read in the outbox and defensively reject empty
  subscription URIs before descendant matching.
- Add deterministic randomized coverage for outbox coalescence/byte accounting and the
  subscription state machine, plus an end-to-end stalled-client regression test.
- Keep native ABI version 1: the stdio options record and entry point are additive and
  no released public layout changed.

## 0.8.0 - 2026-08-15

- Add a public C SDK for `maelys-provider/3` process providers, including JSONL
  dispatch, tool and resource callbacks, descriptor validation, and serialized events.
- Isolate provider stdout by default, preserving the protocol stream when callers
  provide only partial serving options.
- Define callback ownership and shutdown semantics so event-producing provider threads
  stop and join before the SDK is destroyed.
- Keep native ABI version 1: the new SDK handle remains opaque and the public additions
  are source- and binary-compatible.

## 0.7.0 - 2026-08-14

- Replace the private process contract with `maelys-provider/3`: an explicit activation
  barrier followed by id-less resource and catalog change events.
- Add one permanent reader thread per process provider, serialize exchanges, enforce
  response deadlines with condition variables, and fail closed on malformed events or
  unexpected response ids.
- Route provider events through URI validation, subscription filtering, causal
  coalescence and the existing single-writer Outbox.
- Extend the TypeScript and Python provider SDKs with activation-aware, serialized,
  thread-safe event facades; update the example and black-box conformance runner.
- Protect `main` with required CI checks and pull requests, enable secret scanning and
  private vulnerability reporting, and add security and contribution policies.
- Keep native ABI version 1: the provider handle remains opaque and the C additions are
  source- and binary-compatible.

## 0.6.2 - 2026-08-14

- Give queued subscription metadata and completion responses independent Jansson ID
  objects, eliminating a producer/writer reference-count race detected by Linux TSan.
- Keep continuous integration on every branch push while avoiding duplicate runs when
  an already-tested commit is subsequently tagged for release.

## 0.6.1 - 2026-08-14

- Add continuous integration for every push and pull request, covering native, SDK,
  provider, analyzer, sanitizer, fuzzer and supported official MCP conformance gates.
- Isolate release, ASan, TSan and libFuzzer artifacts under separate build profiles so
  instrumentation flags can never leak into a normal build.
- Publish the MIT license at the repository root.
- Define native ABI version 1, expose runtime/package version queries, keep all stateful
  public handles opaque and document the compatibility policy through 1.0.

## 0.6.0 - 2026-08-14

- Add an independently enabled MCP 2026-07-28 Subscriptions module implementing
  `subscriptions/listen` with an acknowledged accepted-filter subset.
- Negotiate Tools and Resources list changes plus canonical resource URI subscriptions;
  omit unsupported Prompt filters instead of advertising an unavailable capability.
- Tag every event and graceful completion with
  `io.modelcontextprotocol/subscriptionId`, and cancel active listens through
  `notifications/cancelled` without producing an unsolicited response.
- Route acknowledgements, resource/tool notifications and completion responses through
  the 0.5 single-writer outbox; keyed updates inherit causal coalescence and bounded
  backpressure.
- Add public event-producer APIs, bounded subscription storage, duplicate-id rejection,
  URI normalization, exact/subresource matching and native/ASan/TSan coverage.

## 0.5.0 - 2026-08-14

- Add a reusable bounded asynchronous outbox with one writer thread and explicit
  Jansson ownership transfer.
- Separate responses and notifications, prioritize responses with configurable 8:1
  anti-starvation scheduling, and batch pointer removal outside the I/O critical path.
- Coalesce keyed notifications while moving replacements to their newest causal
  position; bound both message count and serialized byte budget.
- Route every stdio response through the outbox so no runtime worker or module writes
  directly to the protocol descriptor.
- Add deterministic scheduling, coalescence, writer-failure, bounded-backpressure and
  multi-producer tests under native and sanitizer gates.

## 0.4.0 - 2026-08-14

- Add an independently enabled Resources module implementing `resources/list`,
  `resources/templates/list`, and `resources/read` for both supported MCP eras.
- Extend `maelys-provider/2` and the TypeScript/Python provider SDKs with static
  resources, URI templates, reads, and multi-round resource results.
- Introduce the opaque `maelys-uri` security facade over uriparser: exact-length
  input, RFC 3986 normalization, embedded/encoded NUL rejection, and bounded URI
  template validation.
- Validate text/blob exclusivity, MIME fields, base64 payloads, canonical URIs, and
  optional resource sizes before data crosses the provider boundary.
- Add native, process-provider, SDK, sanitizer, analyzer, and URI fuzz coverage.

## 0.3.0 - 2026-08-14

- Replace implicit method dispatch with an explicit module registry and derive
  advertised capabilities from enabled modules.
- Move Tools out of the protocol core and add an independently enabled MRTR module.
- Replace `maelys-provider/1` with the breaking, explicit `maelys-provider/2` result
  contract; remove the legacy arbitrary-JSON call result.
- Support structured content alongside text, image, audio, resource-link and embedded
  resource blocks with bounded shape, MIME and base64 validation.
- Forward client capabilities, `inputResponses`, and opaque `requestState` across
  provider rounds; reject undeclared required capabilities with MCP error `-32021`.
- Extend native, TypeScript, Python, provider-conformance and official MCP coverage.
- Make provider describe, call and shutdown deadlines independently configurable;
  bound process termination with `SIGTERM` then `SIGKILL` escalation.
- Buffer JSON Lines without losing co-read frames and protect every provider socket
  with close-on-exec before any subsequent provider is launched.
- Reject embedded NUL bytes in protocol discriminants, MIME, base64, URI and opaque
  state fields before they cross a length-unaware C boundary.

## 0.2.0 - 2026-08-14

- Allow provider launchers using `#!/usr/bin/env node` or Python to resolve their
  interpreter from a fixed cross-platform system `PATH` without inheriting the caller
  environment.
- Add dependency-free TypeScript and Python provider SDK packages with descriptor and
  schema validation aligned to the native runtime.
- Add a black-box provider conformance runner covering persistent lifecycle, scenarios,
  structured errors, shutdown and stdout contamination.
- Document stdio client setup for Codex and Claude Code.
- Add a pinned, explicitly partial integration with the official MCP 2026-07-28
  conformance runner for the supported tools-only facade.
- Clarify that TypeScript and Python packages implement only the private provider
  protocol and are not general MCP SDKs.

## 0.1.0 - 2026-08-14

- Add the provider-neutral C runtime and stdio host.
- Support MCP 2026-07-28 and compatibility with MCP 2025-11-25.
- Add persistent external providers with policy effect classes.
- Isolate protocol stdout from library and diagnostic output.
- Reject schema features that the runtime cannot enforce.
- Port generic JSON-RPC, MCP lifecycle, provider, and stdio regression coverage.
- Add digest-pinned Linux ASan/UBSan/LSan validation and three libFuzzer targets.
