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
- Authorization can hide tools and resources from discovery and deny invocation
  independently. Every one of the runtime's five decision points - `tools/list`,
  `tools/call`, `resources/list`, `resources/templates/list` and
  `resources/read` - goes through the middleware chain's `on_authorize` hook,
  which decides on the resolved identity and on the channel's embedder-bound
  principal, never on the client-asserted `clientInfo.name`. A middleware that
  cannot reach a verdict fails the request rather than being read as a denial or
  silently shortening a catalog. See `docs/middleware.md`.
- Policy is decided before arguments are schema-validated, so a denied caller
  cannot map a tool's argument schema through validation error details.
- Transformation is presentation, never privilege. A middleware may rename a
  tool for clients (`on_list`) and map that name back (`on_resolve`), but
  resolution runs *before* the decision, so `on_authorize` always sees the real
  tool and its real effect. `on_list` runs *after* the decision, so a denied
  catalog entry cannot be transformed back into view; an entry it invents has no
  registry backing and a call to it still passes both hooks.
- Arguments are validated against the real tool's input schema, on
  `on_resolve`'s output - never against the schema `on_list` published. A
  provider's result is validated against the real output schema before
  `on_result` may rewrite it.
- `tools/call` and `resources/read` are journalled through the chain's
  `on_audit` hook, denials included and middleware faults with them, with both
  the requested and the resolved identity. The journal carries the **client's**
  params, not the resolved ones, so a value `on_resolve` injected - a hidden
  credential is the canonical case - never reaches an audit sink.
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
- Stdio output writes have a bounded per-message deadline; a non-draining client closes
  the output bus and wakes blocked producers instead of freezing the runtime.
- Every client channel has an independent bounded queue, subscription registry,
  cancellation scope, protocol-era policy and legacy state. Identical JSON-RPC ids on
  different channels cannot collide or receive each other's events, and a channel
  narrowed to one protocol era refuses the other one in the dispatcher rather than
  relying on a transport to filter it out first.
- A channel whose bounded close cannot finish is never freed at its deadline. It is
  aborted, made untargetable and unlinked, and freed only once every operation that
  had already retained it has finished, whether the caller waits for that
  (`maelys_mcp_channel_destroy`) or hands it to the runtime and returns
  (`maelys_mcp_channel_destroy_detached`). `maelys_mcp_runtime_destroy` refuses to
  free a runtime that any such channel still points at.
- Provider event fan-out retains each target channel while delivering outside the
  runtime registry lock. A slow or faulted channel cannot suspend or invalidate its
  peers.
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

The runtime never accepts an executable path, an argument vector, an environment
variable, an execution profile or a shell fragment from an MCP request. All of them
come from host configuration — command-line flags or the manifest — read once at
startup. This holds for every field the launch seam carries, including manifest v2's
`args` and `executionProfile`: they are configuration, never protocol.

### Process externality is the sandbox prerequisite

A provider registered in-process through `maelys_mcp_provider_create` or the
provider SDK runs **inside the runtime's trust boundary**, and no configuration
changes that. It shares the runtime's address space, file descriptors, signal
dispositions and heap. A segmentation fault, a stack overflow, an `abort()` or a
heap corruption in such a provider takes the runtime down with it, and a
memory-safety error in it can read or rewrite any runtime state, including another
provider's. The only failure mode a native in-process provider can express *safely*
is a controlled error return. This is not a defect to be fixed later; it is what
in-process means. The mitigation is a design rule: **a provider whose code is not
trusted at the same level as the runtime must not be in-process.**

Sandboxing requires process externality. Every mechanism worth the name — seccomp,
Seatbelt, Landlock, bubblewrap, containers, separate VMs — operates on a process or
a process tree; none can confine code sharing the confiner's address space. The
runtime therefore reaches sandboxed execution only through the process launch seam
(`docs/launch-contract-design.md`), which starts every child through one vtable,
for both external provider kinds — native `maelys-provider` children and
`mcp-proxy` upstreams. The stock POSIX launcher applies no confinement and says
so: it accepts only an absent `executionProfile` or `"trusted-local"` and refuses
anything else rather than starting it unconfined. A launch path that bypassed the
seam would bypass every future confinement with it, which is why the boundary
audit forbids process-creation primitives outside `src/process/`.

Stdout isolation is a separate and weaker guarantee: a native child can be handed
the protocol on a non-standard descriptor with its stdout wired to stderr; an
`mcp-proxy` upstream cannot, since a third-party MCP server speaks stdin/stdout by
specification. Confinement covers both kinds; descriptor isolation covers only the
kind whose protocol this project defines.

The runtime remains a local stdio host. It does not expose HTTP, authenticate a
network principal or apply per-principal effect policy. Those controls are mandatory
before a network transport can safely expose providers that run with host privileges;
the middleware chain's per-channel context and resolved-identity policy decisions
are the foundation a transport-established principal will bind to.

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
