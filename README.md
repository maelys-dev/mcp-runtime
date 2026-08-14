# Maelys MCP Runtime

Maelys MCP Runtime is a native, policy-enforced MCP host for polyglot developer tools.
It exposes explicitly configured providers through one local MCP endpoint without
embedding provider business logic in the protocol runtime.

The first milestone provides:

- a static C library, `libmaelys_mcp.a`;
- the `maelys-mcp` stdio host;
- injectable in-process providers;
- persistent out-of-process providers using `maelys-provider/1`;
- MCP `2026-07-28` stateless requests and `server/discover`;
- legacy MCP `2025-11-25` initialization for compatibility;
- `tools/list` and `tools/call`;
- input and output validation for a documented JSON Schema subset;
- registration-time rejection of unsupported or ambiguous schema definitions;
- explicit `read`/`preview`/`apply`/`commit`/`execute` authorization classes;
- injectable authorization and audit callbacks;
- an external example provider and conformance tests;
- isolated protocol stdout that cannot be contaminated by ordinary `printf()` calls;
- Linux sanitizer and libFuzzer release gates.

It is deliberately not a shell-command wrapper. Provider executables are absolute,
explicitly configured paths, launched without a shell and with a minimal environment.

## Build and test

Requirements: a C11 compiler, POSIX shell utilities, `make`, `pkg-config`, and Jansson.

```sh
make check
make asan
make test-asan-linux
```

`make test-asan-linux` is the reproducible release gate. It uses a digest-pinned
Ubuntu 24.04/Clang image, enables ASan, UBSan and leak detection, then runs smoke
campaigns for the JSON Lines, Content-Length and schema fuzzers. `make fuzz-smoke`
can also be run directly on a Linux host with Clang/libFuzzer.

For a staged installation or system package build:

```sh
make install DESTDIR=/tmp/mcp-runtime-package PREFIX=/usr/local
```

This installs the public headers, static library, host executable, and
`maelys-mcp.pc` metadata. Providers remain separate executables.

Start the example server:

```sh
build/bin/maelys-mcp \
  --provider "$PWD/build/bin/example-provider"
```

`read` and `preview` tools are enabled by default. Higher-risk classes are opt-in and
independent, for example `--allow-effect apply`; enabling `apply` does not implicitly
enable `commit` or `execute`.

Each MCP request is one JSON object per line on stdin. The host duplicates the original
stdout as a private close-on-exec transport descriptor, then redirects process-wide
stdout to stderr. Consequently, responses alone reach the MCP client even if a linked
library writes diagnostics with `printf()`.

## Repository structure

```text
include/maelys/mcp/  public C API
src/core/            MCP dispatch, registry and schema validation
src/provider/        in-process and process-provider adapters
src/transport/       MCP transports
host/                maelys-mcp executable
providers/example/   independent reference provider
tests/               unit and end-to-end tests
docs/                architecture, security and provider protocol
```

See [Architecture](docs/architecture.md), [Provider protocol](docs/provider-protocol.md),
[Security model](docs/security-model.md), [Protocol support](docs/protocol-support.md),
[Test parity](docs/test-parity.md), and [Provenance](docs/provenance.md).

## Status

Version 0.1.0 establishes the tested local stdio and provider boundary. Streamable
HTTP, cancellation, subscriptions, MRTR, dynamic provider reload, full JSON Schema
2020-12, Windows support and stable ABI guarantees are not implemented yet.
