# Changelog

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
