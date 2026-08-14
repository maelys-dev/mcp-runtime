# Maelys MCP Runtime

[![CI](https://github.com/maelys-dev/mcp-runtime/actions/workflows/ci.yml/badge.svg)](https://github.com/maelys-dev/mcp-runtime/actions/workflows/ci.yml)

Maelys MCP Runtime is a native, policy-enforced MCP host for polyglot developer tools.
It exposes explicitly configured providers through one local MCP endpoint without
embedding provider business logic in the protocol runtime.

The first milestone provides:

- a static C library, `libmaelys_mcp.a`;
- the `maelys-mcp` stdio host;
- injectable in-process providers;
- persistent out-of-process providers using the explicit `maelys-provider/3` result contract;
- provider-originated resource and catalog events over a bidirectional, single-reader
  process channel;
- MCP `2026-07-28` stateless requests and `server/discover`;
- legacy MCP `2025-11-25` initialization for compatibility;
- an explicit module registry with independently enabled Tools, Resources, MRTR and
  Subscriptions modules;
- `tools/list`, `tools/call`, rich text/image/audio/resource content, and
  multi-round `input_required` tool calls;
- `resources/list`, `resources/templates/list`, and `resources/read`, backed by
  static descriptors or bounded URI templates;
- input and output validation for a documented JSON Schema subset;
- registration-time rejection of unsupported or ambiguous schema definitions;
- explicit `read`/`preview`/`apply`/`commit`/`execute` authorization classes;
- injectable authorization and audit callbacks;
- an external example provider and conformance tests;
- isolated protocol stdout that cannot be contaminated by ordinary `printf()` calls;
- a bounded asynchronous output bus with a single protocol writer, response priority,
  anti-starvation and causal notification coalescence;
- MCP 2026-07-28 `subscriptions/listen`, negotiated Tools/Resources filters,
  subscription-tagged change notifications, cancellation and graceful completion;
- Linux sanitizer and libFuzzer release gates.

It is deliberately not a shell-command wrapper. Provider executables are absolute,
explicitly configured paths, launched without a shell and with a minimal environment.

## Build and test

Requirements: a C11 compiler, POSIX shell utilities, `make`, `pkg-config`, Jansson,
and uriparser.

```sh
make check
make check-all
make asan
make tsan
make test-asan-linux
make test-mcp-conformance-official
```

Build profiles never share object files. Normal builds use `build/release`, while
sanitizer and fuzzer targets use `build/asan`, `build/tsan` and `build/fuzz`.
Consequently, running `make tsan` cannot contaminate a later `make check-all`.

`make check` keeps the native C gate dependency-minimal. `make check-all` additionally
runs the TypeScript and Python SDK tests plus the black-box provider conformance suite.
The separately invoked official MCP target uses the upstream HTTP-only alpha runner
through a test adapter and covers only the scenarios matching the implemented modules.
It is not a claim of complete MCP 2026-07-28 conformance.

`make test-asan-linux` is the reproducible release gate. It uses a digest-pinned
Ubuntu 24.04/Clang image, enables ASan, UBSan and leak detection, then runs smoke
campaigns for the JSON Lines, Content-Length, schema, rich-content and URI fuzzers. `make fuzz-smoke`
can also be run directly on a Linux host with Clang/libFuzzer.

For a staged installation or system package build:

```sh
make install DESTDIR=/tmp/mcp-runtime-package PREFIX=/usr/local
```

This installs the public headers, static library, host executable, and
`maelys-mcp.pc` metadata. Providers remain separate executables.

Start the example server:

```sh
build/release/bin/maelys-mcp \
  --provider "$PWD/build/release/bin/example-provider"
```

Provider deadlines can be tuned without changing the provider contract:

```sh
build/release/bin/maelys-mcp \
  --provider "$PWD/build/release/bin/example-provider" \
  --provider-describe-timeout-ms 5000 \
  --provider-call-timeout-ms 300000 \
  --provider-shutdown-timeout-ms 2000
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
src/core/            MCP core, content, URI and schema validation
src/modules/         capability registry, Tools, Resources, MRTR and Subscriptions
src/provider/        in-process and process-provider adapters
src/transport/       MCP transports
host/                maelys-mcp executable
providers/example/   independent reference provider
conformance/          black-box maelys-provider/3 runner and official MCP adapter
sdk/typescript/       dependency-free TypeScript/JavaScript provider SDK
sdk/python/           dependency-free Python provider SDK
tests/               unit and end-to-end tests
docs/                architecture, security and provider protocol
```

See [Architecture](docs/architecture.md), [Provider protocol](docs/provider-protocol.md),
[Security model](docs/security-model.md), [Protocol support](docs/protocol-support.md),
[C API and ABI policy](docs/abi-policy.md),
[Asynchronous outbox](docs/outbox.md), [Subscriptions](docs/subscriptions.md),
[Test parity](docs/test-parity.md), and [Provenance](docs/provenance.md).
Client setup examples are in [Codex and Claude clients](docs/clients.md).
The scope and limitations of the upstream test suite are in
[Official MCP conformance](docs/official-conformance.md).

## Status

Version 0.7.0 adds the bidirectional `maelys-provider/3` process protocol. Providers
activate only after the runtime output bus is ready, then may emit resource updates and
Tools/Resources list changes independently of normal responses. A dedicated reader
thread demultiplexes events and the single in-flight response; SDK writers serialize
all provider output. Provider calls remain synchronous in this release. Streamable
HTTP, prompts, Tasks, progress, dynamic provider reload, full JSON Schema
2020-12 and Windows support are not implemented yet. The pre-1.0 ABI policy is
documented and versioned; same-major ABI stability begins with 1.0.

The source is available under the [MIT License](LICENSE).
