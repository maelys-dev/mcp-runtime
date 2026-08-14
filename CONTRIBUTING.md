# Contributing

## Development setup

Install a C11 compiler, Make, pkg-config, Jansson, uriparser, Python 3 and Node.js.
On macOS the native dependencies are available through Homebrew; the CI workflow is
the reference Ubuntu environment.

Run the complete local gate before opening a pull request:

```sh
make clean
make check-all
make analyze CC=clang ANALYZER=clang
make asan CC=clang
make tsan CC=clang
make fuzz-smoke FUZZ_CC=clang
```

## Pull requests

All changes target a branch and are merged through a pull request. `main` requires
the native/SDK checks, official MCP scenarios, sanitizers and fuzzing. Force-pushes
and branch deletion are disabled on `main`.

Keep protocol changes explicit: update the protocol document, both provider SDKs,
the example provider, conformance fixtures and the changelog in the same pull request.
Never weaken a runtime validator merely to accept an SDK output.

## Ownership and compatibility

`mcp-runtime` owns the native runtime, private provider protocol and reusable provider
SDKs. Product-specific providers remain in their product repositories—for example,
the Hermes provider belongs to `yavena-hermes`. Follow the
[C API and ABI policy](docs/abi-policy.md) for public headers.
