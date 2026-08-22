# Maelys MCP Runtime

[![CI](https://github.com/maelys-dev/mcp-runtime/actions/workflows/ci.yml/badge.svg)](https://github.com/maelys-dev/mcp-runtime/actions/workflows/ci.yml)

A native C host for the [Model Context Protocol](https://modelcontextprotocol.io).
It sits between an AI client (Claude Code, Codex, …) and the tools that client
calls, and puts the operator in control: every tool is classed by effect,
schema-validated, authorized by your policy, and journalled — and tools from
many providers, including third-party MCP servers, are exposed through one
local endpoint.

It is deliberately **not** a shell-command wrapper. Providers are separate
executables at explicitly configured absolute paths, launched without a shell
and with a minimal environment, speaking an explicit wire contract.

## What it gives you

- **One endpoint, many providers.** Register in-process providers, external
  process providers (with dependency-free C, TypeScript and Python SDKs), or
  federate any third-party MCP server through the built-in
  [MCP proxy](docs/mcp-proxy.md) — its tools get the same gating, validation,
  policy and audit as native ones. Declare a whole fleet in one JSON
  [manifest](docs/manifest.md) (`--manifest`).
- **Guardrails on by default.** Every tool carries an effect class —
  `read`, `preview`, `apply`, `commit`, `execute` — and only the first two run
  without an explicit `--allow-effect`. Inputs and outputs are validated
  against a documented JSON Schema subset, and schemas the runtime cannot
  enforce are rejected at registration, not silently accepted.
- **A programmable middleware chain** ([docs](docs/middleware.md)). Seven
  hooks let embedders decide and transform everything that crosses the
  runtime: authorize each of the five decision points, journal with both the
  requested and the resolved identity, rename tools and inject arguments the
  client never sees, short-circuit calls (cache, proxy, refusal), redact
  results, wrap a request's output stream, and rewrite the advertised
  catalogs.
- **Modern and legacy MCP.** Stateless `2026-07-28` with `server/discover`,
  plus negotiated legacy `initialize` for `2024-11-05` through `2025-11-25`.
  Tools, Resources, Subscriptions and multi-round `input_required`
  (elicitation, sampling, roots) as independently enabled modules;
  per-request `notifications/progress` in strict order ahead of the response.
- **Built like infrastructure.** Protocol stdout is isolated so a stray
  `printf()` cannot corrupt the stream; per-client channels have bounded
  queues, response priority and anti-starvation; CI gates every change under
  clang *and* GCC, ASan/UBSan/TSan, libFuzzer campaigns and the official MCP
  conformance runner; releases ship with SHA-256 checksums and build
  provenance attestations.

## Install

On macOS or Linux, via the Homebrew tap:

```sh
brew install maelys-dev/tap/mcp-runtime
```

This builds from source and installs the `maelys-mcp` host, the static library
`libmaelys_mcp.a`, and the public headers.

Alternatively, download a prebuilt tarball from the
[latest release](https://github.com/maelys-dev/mcp-runtime/releases/latest):
each platform ships a `-static` archive (standalone, bundles jansson and
uriparser) and a `-dynamic` one (links them from the system), with a `.sha256`
and a provenance attestation verifiable via `gh attestation verify`.
Windows is not supported (the runtime is POSIX-only).

## Quick start

Serve one provider over stdio:

```sh
maelys-mcp --provider "$PWD/build/release/bin/example-provider"
```

Allow a higher-risk effect class (each is opt-in and independent — enabling
`apply` does not enable `commit` or `execute`):

```sh
maelys-mcp \
  --provider "$PWD/build/release/bin/example-provider" \
  --allow-effect apply
```

Or declare several providers — native and federated MCP servers alike — in
one manifest:

```sh
maelys-mcp --manifest /etc/maelys/providers.json
```

Provider deadlines are tunable without changing any provider
(`--provider-describe-timeout-ms`, `--provider-call-timeout-ms`,
`--provider-shutdown-timeout-ms`). Client setup examples for Codex and Claude
Code are in [docs/clients.md](docs/clients.md).

## Build and test

Requirements: a C11 compiler, POSIX shell utilities, `make`, `pkg-config`,
Jansson, and uriparser.

```sh
make check                          # native C gate, dependency-minimal
make check-all                      # + TypeScript/Python SDK and provider conformance
make asan                           # ASan + UBSan
make tsan                           # ThreadSanitizer
make test-asan-linux                # reproducible digest-pinned Linux release gate
make test-mcp-conformance-official  # official MCP runner, supported scenarios
```

Build profiles never share object files (`build/release`, `build/asan`,
`build/tsan`, `build/fuzz`), so running a sanitizer target cannot contaminate
a later normal build. The official conformance target covers the scenarios
matching the implemented modules; it is not a claim of complete MCP
conformance.

For a staged installation or system package build:

```sh
make install DESTDIR=/tmp/mcp-runtime-package PREFIX=/usr/local
```

This installs the public headers, static library, host executable, and
`maelys-mcp.pc` metadata. Providers remain separate executables.

## Repository structure

```text
include/maelys/mcp/  public C API
src/core/            MCP core, middleware chain, content, URI and schema validation
src/modules/         capability registry, Tools, Resources, MRTR and Subscriptions
src/process/         the one process launch seam and its POSIX launcher
src/provider/        in-process, process-provider and MCP proxy adapters
src/transport/       MCP transports
host/                maelys-mcp executable
providers/example/   independent reference provider
conformance/         black-box maelys-provider runner and official MCP adapter
sdk/c/               public C process-provider SDK notes
sdk/typescript/      dependency-free TypeScript/JavaScript provider SDK
sdk/python/          dependency-free Python provider SDK
tests/               unit and end-to-end tests
docs/                architecture, security and provider protocol
```

## Documentation

[Architecture](docs/architecture.md) ·
[Middleware chain](docs/middleware.md) ·
[Security model](docs/security-model.md) ·
[MCP proxy provider](docs/mcp-proxy.md) ·
[Host provider manifest](docs/manifest.md) ·
[Provider protocol](docs/provider-protocol.md) ·
[Protocol support](docs/protocol-support.md) ·
[C API and ABI policy](docs/abi-policy.md) ·
[Passive outbox](docs/outbox.md) ·
[Subscriptions](docs/subscriptions.md) ·
[Test parity](docs/test-parity.md) ·
[Mutation testing](docs/mutation-testing.md) ·
[Provenance](docs/provenance.md) ·
[Official MCP conformance](docs/official-conformance.md) ·
[Codex and Claude clients](docs/clients.md)

## Status

Pre-1.0, native ABI 4. The 0.18 line moves the channel's protocol-era mask into
`maelys_mcp_channel_config_t` and gives the channel context a destructor
(migration is a recompile — see [the ABI policy](docs/abi-policy.md)). The 0.15
line adds nested (in-band) multi-round-trip
requests — a tool can ask the client a question mid-call, on both modern and
legacy MCP — with concurrent calls on one connection, and ships a mutation
testing runner ([docs](docs/mutation-testing.md)) whose first sweeps' 128
surviving mutants are the named test-hardening backlog. The 0.14 line
completed the seven-hook middleware chain, replacing the former
`authorize`/`audit` config callbacks (migration is one call — see the
[CHANGELOG](CHANGELOG.md)). The `maelys-provider` wire protocol is negotiated
(`/3` floor, `/4` progress, `/5` nested requests; every version in the range
is accepted), so existing providers keep working across host upgrades. All
three bundled SDKs expose nested requests since 0.16, declaring `/5` only
once a provider actually nests so a non-nesting provider stays byte-identical
on the wire.

**The HTTP endpoint serves MCP as of the 0.19 line, over the `2026-07-28` era
only.** `--http-listen` dispatches: a channel per `POST` bound to that request's
authenticated principal, `application/json` for a request that resolves to one
response and `text/event-stream` for one that streams, `202` for a
notification, and cancellation when the client disconnects. Legacy protocol
support stays stdio-only, and one process serves both at once. What is not
claimed yet is **Streamable HTTP**: the official conformance suite still runs
through a test-only adapter rather than against this listener, and the claim
waits until it does — see
[protocol support](docs/protocol-support.md).

Not implemented yet: Streamable HTTP conformance (the transport ships; the
claim does not), prompts, Tasks, dynamic provider reload, full JSON Schema
2020-12, Windows.
The pre-1.0 ABI policy is documented and versioned; same-major ABI stability
begins with 1.0.

The source is available under the [MIT License](LICENSE).
