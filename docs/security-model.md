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
  free a runtime that any such channel still points at. A channel context that owns
  something is handed back at that free through
  `maelys_mcp_channel_config_t::context_release`, exactly once and whichever path
  destruction took, so a detached channel's context — an authenticated principal, in
  the case the callback was designed for — is released as soon as it stops being
  reachable rather than being pinned for the runtime's remaining lifetime.
- Provider event fan-out retains each target channel while delivering outside the
  runtime registry lock. A slow or faulted channel cannot suspend or invalidate its
  peers.
- Protocol fields that enter length-unaware C APIs reject embedded NUL bytes.
- Resource URIs are length-bounded, parsed and normalized behind an opaque uriparser
  facade; embedded NUL and encoded `%00` are rejected before provider dispatch.
- Resource contents require exactly one of text/blob, valid bounded base64, and a
  canonical URI. Parsing never grants filesystem or network access by itself.
- Provider descriptors are close-on-exec and provider termination is bounded.
- The HTTP listener binds `127.0.0.1` by default and is off unless `--http-listen`
  asks for it. Binding any other address refuses to start without an authenticator
  other than loopback trust, and refuses before the socket exists rather than after.
  It serves MCP over `2026-07-28` only (`docs/protocol-support.md`), and that
  restriction is structural rather than a policy: an HTTP channel is created
  with the modern era alone, so `initialize` is refused by the runtime and no
  request can negotiate a legacy revision on it. Legacy support stays stdio-only.
- Every `POST` gets its own channel and its own authentication. A kept-alive
  connection is a transport optimisation and never a session: the credential is
  re-checked per request, the principal is bound to that request's channel
  alone, and nothing about one request survives into the next.
- A principal reference is released exactly once, at the moment its channel
  stops being reachable — which for a channel that missed its close deadline is
  after the connection has already been let go. That is what the channel context
  destructor exists for, and it is why a wedged provider cannot make a
  connection slot, or a principal, outlive its own bound.
- `Origin` is validated on every request, before the body is read, before
  authentication, and before anything else. The allowlist is empty by default and a
  request with no `Origin` is accepted only on a loopback bind. This is the
  DNS-rebinding control and it is the check that runs earliest.
- `Host` must be present exactly once and syntactically valid, and on a loopback bind
  must be a loopback authority. An absolute-form request target whose authority
  disagrees with `Host` is refused rather than resolved in favour of either.
- Request framing is unambiguous by refusal rather than by resolution. Any
  `Transfer-Encoding` is rejected outright; a `Transfer-Encoding` together with a
  `Content-Length` is rejected and logged as a smuggling attempt; a repeated or
  non-`1*DIGIT` `Content-Length` is rejected even when duplicates agree; `obs-fold`
  continuations and bare-LF line endings are rejected. Every framing rejection closes
  the connection, and the parser never resynchronizes to look for a following request.
- Any header value containing NUL, CR or LF is refused before it is interpreted, and
  a repeated protocol header is refused rather than merged.
- Request line, header block, header count and declared body length are independently
  bounded, and the body bound is the same `max_message_bytes` the stdio reader and the
  channel output budget derive from. Header reads and body reads have their own
  deadlines.
- Request pipelining is refused: any inbound byte arriving between a request and the
  completion of its response terminates the connection. Connection reuse is permitted
  for `application/json` replies and refused for event streams.
- The credential is checked as soon as the request headers are complete and before any
  request body is read, so an unauthenticated caller can neither hold a server thread
  for the body deadline nor cause a body-sized allocation. A rejected request is
  answered and closed after discarding at most 8 KiB of unread body under a short
  deadline, which is what makes the answer reach the caller instead of being lost to a
  reset. Authentication is repeated for every POST, so two requests on one kept-alive
  connection are two independent authentications.
- An absent or invalid credential is answered `401` with `WWW-Authenticate: Bearer`,
  never `403`. An authenticator that cannot reach a verdict fails the request with
  `503`; it is never read as anonymous and never read as a denial.
- The transport principal is established from a credential the transport itself
  authenticated and is never derived from payload metadata. `clientInfo.name`, `_meta`
  and the client-written `Mcp-Method` / `Mcp-Name` / `Mcp-Param-*` routing headers are
  advisory and carry no authority. See `docs/authenticated-principal-design.md`.
- `static-bearer` is a test and single-tenant mechanism. A runtime whose only network
  credential is a shared secret in a config file is not a multi-tenant server, and
  multi-tenant MRTR continuations are not supported: only `loopback-trust` and a
  genuinely mono-tenant single-token configuration may be described as safe.
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

The runtime serves MCP over stdio and over HTTP. The listener authenticates a
network principal, binds it to the channel that serves the request, and dispatches;
the official conformance runner drives that endpoint directly
(`docs/official-conformance.md`). Of the two controls this paragraph named as
mandatory before a network transport can safely expose providers that run with host
privileges, **one is done and one is not**.

Done: the listener serves MCP, and every request is attributed to a principal the
transport itself authenticated rather than to anything the payload asserted.

Not done: **the effect policy is still runtime-wide rather than per-principal.**
`--allow-effect` enables an effect for the process, not for a caller, so every
authenticated principal that reaches a tool gets the same effect set. The mechanism
for fixing that exists and is unused — the middleware chain's per-channel context is
where a transport-established principal binds, and ABI 4's `context_release` is what
lets a per-request transport own one safely — but no middleware in this repository
reads it yet. Until one does, an HTTP deployment that enables `apply`, `commit` or
`execute` grants those effects to **every** principal the authenticator admits, and
a single shared bearer token is therefore a single shared privilege level. That is
the control to build before exposing this listener to more than one caller.

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
