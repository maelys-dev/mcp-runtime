# Changelog

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
