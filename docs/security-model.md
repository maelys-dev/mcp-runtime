# Security model

## Defaults

- Only explicitly configured provider executables are launched.
- Provider paths must be absolute.
- Providers are executed directly, never through a shell.
- Provider children receive only `PATH=/usr/bin:/bin`, `LANG=C`, and `LC_ALL=C`.
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
- Request and provider message sizes are bounded.

## Trust boundary

Configured provider binaries are trusted local code. The minimal environment prevents
accidental credential inheritance but does not sandbox filesystem, network, CPU, or
memory access. A later sandbox adapter may use OS facilities or containers.

The runtime never accepts an executable path, argv, environment variable, or shell
fragment from an MCP request.

## Required provider practices

- enforce repository-root and path constraints in the application layer;
- separate inspection, preview, apply, and commit tools;
- make mutation opt-in and auditable;
- return structured errors without secrets;
- verify optimistic revisions or preview tokens before writes.
