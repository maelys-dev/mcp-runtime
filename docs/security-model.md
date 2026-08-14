# Security model

## Defaults

- Only explicitly configured provider executables are launched.
- Provider paths must be absolute.
- Providers are executed directly, never through a shell.
- Provider children receive only a fixed platform-specific system `PATH`, `LANG=C`,
  and `LC_ALL=C`; the caller's environment is never inherited.
- Tool name collisions fail closed.
- Unknown tools and malformed arguments are rejected before provider invocation.
- Tools declare one mandatory effect: `read`, `preview`, `apply`, `commit`, or `execute`.
- `apply`, `commit`, and `execute` are denied by the reference host unless
  each effect is explicitly enabled with `--allow-effect`.
- Authorization can hide tools from discovery and deny invocation independently.
- The host retains the original stdout in a private `FD_CLOEXEC` descriptor and
  redirects process-wide stdout to stderr before provider or runtime initialization.
  Third-party `printf()` output therefore cannot contaminate MCP responses.
- Schemas with unsupported keywords or inconsistent constraints fail registration.
- Provider output that violates its announced schema is discarded and returned only
  as an MCP tool error.
- Rich content is shape-checked; image/audio and resource blobs must be bounded valid
  base64 and use the matching MIME family.
- MRTR input requests are rejected when the client omitted their required capability.
- `requestState` is opaque to the runtime. A mutating provider must authenticate and
  bind it to the operation, arguments, expected round and expiration before acting.
- Request and provider message sizes are bounded.
- Protocol fields that enter length-unaware C APIs reject embedded NUL bytes.
- Resource URIs are length-bounded, parsed and normalized behind an opaque uriparser
  facade; embedded NUL and encoded `%00` are rejected before provider dispatch.
- Resource contents require exactly one of text/blob, valid bounded base64, and a
  canonical URI. Parsing never grants filesystem or network access by itself.
- Provider descriptors are close-on-exec and provider termination is bounded.
- Process-provider output has one dedicated reader. Event envelopes require protocol
  v3, no id, a known method and strictly typed parameters; response ids must match the
  single outstanding exchange.

## Trust boundary

Configured provider binaries are trusted local code. The fixed environment prevents
accidental credential inheritance but does not sandbox filesystem, network, CPU, or
memory access. Its non-inherited `PATH` contains only standard Linux, Intel macOS and
Apple Silicon Homebrew locations, allowing portable `#!/usr/bin/env node` and Python
launchers to resolve their interpreter. A later sandbox adapter may use OS facilities
or containers.

The runtime never accepts an executable path, argv, environment variable, or shell
fragment from an MCP request.

## Required provider practices

- enforce repository-root and path constraints in the application layer;
- separate inspection, preview, apply, and commit tools;
- make mutation opt-in and auditable;
- return structured errors without secrets;
- verify optimistic revisions or preview tokens before writes.
- treat accepted elicitation as an authorization input, not as a replacement for the
  runtime effect policy or the provider's own invariants.
- resolve resource URIs inside an application-specific allowlisted root; the generic
  URI facade deliberately does not turn a syntactically valid URI into an I/O right.
- emit events only after `provider/activate`; event delivery is an invalidation signal,
  not proof that a mutation was authorized or completed.
