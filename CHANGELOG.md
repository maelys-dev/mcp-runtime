# Changelog

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
