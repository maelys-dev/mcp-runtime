# Changelog

## Unreleased

- **Middleware chain**, the runtime's single policy and observation seam. The
  design in `docs/middleware-design.md` specifies seven hooks; this lands the chain
  core — registration, ordering, invocation — and its two decision points,
  `on_authorize` and `on_audit`. All five of the runtime's policy decision points go
  through it: `tools/list` (per tool), `tools/call`, `resources/list` (per resource),
  `resources/templates/list` (per template) and `resources/read`. Decisions are taken
  on the resolved identity and on the channel's embedder-bound opaque pointer, and a
  hook that cannot reach a verdict is distinct from one that denies. See
  `docs/middleware.md`.
- **API break (ABI 2 → 3): `maelys_mcp_runtime_config_t` loses `authorize`, `audit`
  and `policy_context`.** The chain replaces those callbacks rather than coexisting
  with them; two similar-looking mechanisms on the security-critical path double the
  surface that has to be audited. **Migration is one call**:
  `maelys_mcp_runtime_add_compat_policy(runtime, authorize, audit, policy_context)`
  registers a built-in middleware that calls the same callbacks with the same
  metadata, before the first channel. Registering just an authorizer is one struct
  field. `maelys_mcp_request_context_t`, `maelys_mcp_authorize_fn` and
  `maelys_mcp_audit_fn` moved from `maelys/mcp/runtime.h` to
  `maelys/mcp/middleware.h`, which `maelys/mcp.h` includes.
- **Named behaviour change: policy now runs before schema validation on
  `tools/call`.** A denied caller returns `-32003` with no data whether or not its
  arguments would have validated, so it cannot map a tool's argument schema through
  validation error details. The consequence for middleware authors is in the public
  contract: `on_authorize` sees params this runtime has not validated yet.
- **Named behaviour change: a denied `resources/read` is journalled**, as a denied
  `tools/call` always was. The asymmetry was an omission.
- **Named behaviour change: an undecidable verdict is not a denial.**
  `MAELYS_MCP_AUTHORIZE_ERROR` maps to `-32603`, never `-32003`, and fails a whole
  listing rather than dropping the entry it could not evaluate — a hidden catalog
  entry and an unevaluated one are indistinguishable to a client.
- `on_authorize` receives the request's whole params, `inputResponses` and
  `requestState` included, so MRTR continuation traffic — elicitation answers and
  sampling completions — is no longer invisible to policy. This is visibility, not
  binding: a continuation can still arrive on a different channel than the one that
  issued it.
- `on_audit` carries both the requested and the resolved identity, so a journal
  records what the client asked for rather than what a future transform injected.
- **Tool renaming and argument injection** (`on_resolve`, hook ①). You can now
  publish a tool under a different name, hide an argument from clients and supply
  it yourself, or map a whole family of published names onto one real tool — the
  client calls `edit_doc`, the runtime runs `hermes.content.apply` with an API key
  the client never saw. The rename resolves *before* the policy decision, so
  re-exposing an apply-effect tool under a gentler name is still an apply as far as
  `on_authorize` is concerned. Arguments are validated against the real tool's
  schema, so a transform that changes an argument's **type** must convert it here.
- **Catalog transformation** (`on_list`, hook ⑦). You can now rewrite what
  `tools/list`, `resources/list` and `resources/templates/list` return: rename
  entries, rewrite their schemas or descriptions, filter them per principal, or add
  synthetic entries no provider registered — the catalog half of a proxy or a
  retrieval-first meta-tool. It runs after `on_authorize` has filtered, so a denied
  entry cannot be transformed back into view, and a synthetic name still passes
  `on_resolve` and `on_authorize` when it is called.
- **Answering a call without reaching the provider** (`on_call`, hook ③). You can
  now short-circuit a tool call from a middleware — a cache hit, a proxied
  upstream, a canned refusal — and the provider is never invoked. Arguments are
  read-only here by design: rewriting them is `on_resolve`'s job, because arguments
  mutated at this point would be mutated after schema validation. A substituted
  result is validated exactly like a provider's, output schema included.
- **Result rewriting and redaction** (`on_result`, hook ④). You can now redact,
  truncate or rewrite a tool result before it reaches the client. Several hooks
  compose in registration order. The runtime validates the **provider's** result
  against the real output schema before this hook runs; your replacement is
  re-checked for structural and content-block validity but not against that schema,
  because dropping a field the schema requires is what redaction is — keeping
  `on_list`'s advertised `outputSchema` consistent with what you emit is yours to
  do, and the schema is handed to the hook so you can.
- **Decorating a request's outbound frames** (`wrap_sink`, hook ⑥). You can now
  wrap the delivery path a single request's output travels — every
  `notifications/progress` frame it emits and its final response — for progress
  coalescing, stream redaction or per-request accounting. Every member of the
  wrapper is optional and a NULL one forwards untouched. Progress is still
  delivered strictly ahead of the response, and the runtime enforces the two ways a
  wrapper could break a request: a swallowed response is detected and answered past
  the chain with `-32603` rather than left to wedge the connection, and a second
  completion for one id is refused.
- `maelys_mcp_middleware_t` gains `on_resolve`, `on_call`, `on_result`, `wrap_sink`
  and `on_list`, and its fields are now in hook order. This extends the ABI-3
  descriptor introduced above before ABI 3 has been released, so there is no
  further bump; every field is optional and existing designated initializers keep
  compiling unchanged.
- `maelys_mcp_response_sink_t` becomes a public **opaque** type, with
  `maelys_mcp_sink_emit`, `maelys_mcp_sink_complete` and
  `maelys_mcp_sink_cancelled` to forward through it. The layout stays private, so
  decorating the delivery path is not a commitment to its shape.
- The middleware suite now runs under `make tsan` as well as `make check`, because
  `wrap_sink` sits on the path every request's output takes.

## 0.13.1 - 2026-08-20

- **Fix**: the release build's Linux (GCC) runners failed on
  `-Werror=format-truncation=` in `host/manifest.c` — GCC sizes `%zu` at its
  theoretical worst case (up to 20 digits for a 64-bit `size_t`), which
  exceeded the buffer used to format `allowEffects[N]` error locations. clang
  (the regular CI checks, local macOS builds) never flagged it, so it shipped
  undetected in 0.13.0. No behavioural change; v0.13.0's tag was never
  followed by a GitHub Release (its Linux builds failed, so publishing was
  skipped) — this is the first release actually carrying 0.13.0's features.

## 0.13.0 - 2026-08-20

- **MCP proxy provider**: federate any third-party MCP server over stdio. mcp-runtime
  spawns it, negotiates its era (modern `2026-07-28` or a legacy dated revision),
  snapshots its tool catalog once at connect, and re-exposes those tools through the
  normal provider pipeline — effect gating, schema validation, policy and audit all
  apply to traffic this runtime did not originate. The catalog snapshot is pinned:
  calls resolve only against it, never against a fresh list, so a renamed or
  disappeared upstream tool cannot be silently substituted. Progress notifications
  relay end to end, correlated on the caller's own client token, never the upstream's.
- **Schema policy for the proxy**: an upstream tool whose `inputSchema` this runtime's
  strict validator cannot handle no longer has to fail the whole connection. Three
  policies — `strict` (default: fails connect, unchanged from initial release),
  `skip` (the tool is dropped from the catalog and reported by name; the rest of the
  upstream stays usable), `passthrough` (the tool is exposed with a permissive schema
  and the upstream validates its own arguments; effect gating, policy and audit still
  apply).
- **Provider manifest**: `--manifest /absolute/path.json` declares a whole provider
  set — native and proxy providers alike, plus `allowEffects` — instead of one
  `--provider` flag per process. Strict two-phase validation rejects any unknown key
  by name and location; nothing is constructed from a partially validated document.
  Composes with `--provider`/`--allow-effect` rather than replacing them. See
  `docs/manifest.md`.
- **`notifications/progress` support**, end to end. In-process and out-of-process
  providers can report progress on a long-running call; the bare `_meta.progressToken`
  key (identical across both protocol eras) opts a call in, and progress frames are
  delivered in strict order ahead of the call's final response — including over the
  provider wire, which required negotiating `maelys-provider/3` → `/4` (a provider
  declares its own version in responses; the host never requires an upgrade, so an
  unmodified `/3` provider keeps working exactly as before). Closes the official
  conformance suite's `tools-call-with-progress` scenario in both the modern and
  legacy requirement sets.
- **Response sink**: dispatch now delivers through a transport-neutral sink
  (`emit`/`complete`/`cancelled`) instead of assuming a single buffered reply. Pure
  internal refactor with no behavioural change on its own — the foundation the
  progress and (future) HTTP streaming work build on.
- Official conformance coverage extended to the legacy `2025-11-25` requirement set,
  fetched live from the official tool rather than hand-copied, plus a permanent
  concurrent-dispatch test and channel/provider performance baselines that now run on
  every `make check`/`make tsan`.

## 0.12.2 - 2026-08-17

- Legacy clients (`2024-11-05` through `2025-11-25`) can now use `input_required`/MRTR
  (elicitation, sampling, roots) exactly like modern clients, provided they declared
  the relevant capability once at `initialize` — the same top-level keys
  (`elicitation`, `sampling`, `roots`) that modern clients declare per-request in
  `_meta`. Previously every legacy channel was unconditionally denied with
  `input_required requires the modern MCP protocol and MRTR module`, regardless of
  what it had declared. Applies to both `tools/call` and `resources/read`. A legacy
  channel that declared nothing keeps getting `-32021`/`requiredCapabilities`, same
  as before.

## 0.12.1 - 2026-08-17

- Fix a regression where any request carrying an opaque `_meta` object (for example
  a progress token) on an already legacy-initialized channel was incorrectly forced
  through modern `protocolVersion`/`clientCapabilities` validation and rejected with
  `-32602`. Only `_meta` that actually carries the
  `io.modelcontextprotocol/protocolVersion` key is now treated as a negotiation
  attempt — including on a legacy-initialized channel, where it is correctly
  rejected as an unsupported mid-session renegotiation. Fixes a startup failure
  observed with legacy MCP clients (e.g. Hermes) sending per-request metadata.

## 0.12.0 - 2026-08-17

- Negotiate the legacy `initialize` handshake instead of requiring an exact match on
  `2025-11-25`. The runtime now accepts `2024-11-05`, `2025-03-26`, `2025-06-18` and
  `2025-11-25`, and echoes the client's requested version back in the result, so
  clients (such as Codex) that announce an older dated revision are no longer rejected
  with `-32602`. `2026-07-28` support (stateless, via per-request `_meta`) is unchanged.

## 0.11.0 - 2026-08-17

- Add a tag-triggered binary release pipeline (`.github/workflows/release.yml`)
  with separated build and publish roles: the privileged publish job runs no
  candidate code, only verifies checksums and attaches artifacts.
- Ship per-platform static and dynamic tarballs (Linux x86_64/arm64, macOS
  arm64/x86_64) via `scripts/package-release.sh`, each with a SHA-256 checksum
  and a build provenance attestation.
- Move `VERSION` into a data file read by both `make` and the release workflow.

## 0.10.0 - 2026-08-15

- Introduce opaque `maelys_mcp_channel_t` handles. Each channel owns its ordered
  output queue, subscriptions, cancellation scope and legacy client lifecycle.
- Replace the runtime-wide writer outbox with passive bounded per-channel queues;
  transports now pump output through deadline-aware `maelys_mcp_channel_next`.
- Route every response, protocol error, listen acknowledgement, provider event and
  graceful completion through the channel queue. A response admission timeout faults
  only that channel; notification coalescence and rejection remain local.
- Make provider event fan-out multi-channel with stable per-channel references and
  local close barriers. Identical JSON-RPC subscription ids on different channels no
  longer collide or permit cross-delivery.
- Make provider activation runtime-scoped, once-only and fail-closed. Make
  `maelys_mcp_runtime_destroy` return `MAELYS_MCP_ERR_STATE` while a channel handle
  remains alive, including a closed handle not yet destroyed.
- Remove `maelys_mcp_runtime_handle`, `maelys_mcp_runtime_attach_outbox` and
  `maelys_mcp_runtime_detach_outbox` without compatibility wrappers. Migrate by
  creating a channel, passing requests to `maelys_mcp_channel_handle`, pumping output
  with `maelys_mcp_channel_next`, then closing and destroying the channel before the
  runtime.
- Increment the native ABI from 1 to 2. The `maelys-provider/3` wire protocol and C,
  TypeScript and Python provider SDK contracts are unchanged.

## 0.9.0 - 2026-08-15

- Bound stdio message writes with a configurable monotonic deadline and wake the
  service loop when the writer fails, preventing a non-draining client from freezing
  the runtime and its producers indefinitely.
- Add the backward-compatible `maelys_mcp_runtime_serve_stdio_with_options` API and
  `--stdio-write-timeout-ms` host option; the existing API uses a five-second default.
- Eliminate an unlocked `writer_status` read in the outbox and defensively reject empty
  subscription URIs before descendant matching.
- Add deterministic randomized coverage for outbox coalescence/byte accounting and the
  subscription state machine, plus an end-to-end stalled-client regression test.
- Keep native ABI version 1: the stdio options record and entry point are additive and
  no released public layout changed.

## 0.8.0 - 2026-08-15

- Add a public C SDK for `maelys-provider/3` process providers, including JSONL
  dispatch, tool and resource callbacks, descriptor validation, and serialized events.
- Isolate provider stdout by default, preserving the protocol stream when callers
  provide only partial serving options.
- Define callback ownership and shutdown semantics so event-producing provider threads
  stop and join before the SDK is destroyed.
- Keep native ABI version 1: the new SDK handle remains opaque and the public additions
  are source- and binary-compatible.

## 0.7.0 - 2026-08-14

- Replace the private process contract with `maelys-provider/3`: an explicit activation
  barrier followed by id-less resource and catalog change events.
- Add one permanent reader thread per process provider, serialize exchanges, enforce
  response deadlines with condition variables, and fail closed on malformed events or
  unexpected response ids.
- Route provider events through URI validation, subscription filtering, causal
  coalescence and the existing single-writer Outbox.
- Extend the TypeScript and Python provider SDKs with activation-aware, serialized,
  thread-safe event facades; update the example and black-box conformance runner.
- Protect `main` with required CI checks and pull requests, enable secret scanning and
  private vulnerability reporting, and add security and contribution policies.
- Keep native ABI version 1: the provider handle remains opaque and the C additions are
  source- and binary-compatible.

## 0.6.2 - 2026-08-14

- Give queued subscription metadata and completion responses independent Jansson ID
  objects, eliminating a producer/writer reference-count race detected by Linux TSan.
- Keep continuous integration on every branch push while avoiding duplicate runs when
  an already-tested commit is subsequently tagged for release.

## 0.6.1 - 2026-08-14

- Add continuous integration for every push and pull request, covering native, SDK,
  provider, analyzer, sanitizer, fuzzer and supported official MCP conformance gates.
- Isolate release, ASan, TSan and libFuzzer artifacts under separate build profiles so
  instrumentation flags can never leak into a normal build.
- Publish the MIT license at the repository root.
- Define native ABI version 1, expose runtime/package version queries, keep all stateful
  public handles opaque and document the compatibility policy through 1.0.

## 0.6.0 - 2026-08-14

- Add an independently enabled MCP 2026-07-28 Subscriptions module implementing
  `subscriptions/listen` with an acknowledged accepted-filter subset.
- Negotiate Tools and Resources list changes plus canonical resource URI subscriptions;
  omit unsupported Prompt filters instead of advertising an unavailable capability.
- Tag every event and graceful completion with
  `io.modelcontextprotocol/subscriptionId`, and cancel active listens through
  `notifications/cancelled` without producing an unsolicited response.
- Route acknowledgements, resource/tool notifications and completion responses through
  the 0.5 single-writer outbox; keyed updates inherit causal coalescence and bounded
  backpressure.
- Add public event-producer APIs, bounded subscription storage, duplicate-id rejection,
  URI normalization, exact/subresource matching and native/ASan/TSan coverage.

## 0.5.0 - 2026-08-14

- Add a reusable bounded asynchronous outbox with one writer thread and explicit
  Jansson ownership transfer.
- Separate responses and notifications, prioritize responses with configurable 8:1
  anti-starvation scheduling, and batch pointer removal outside the I/O critical path.
- Coalesce keyed notifications while moving replacements to their newest causal
  position; bound both message count and serialized byte budget.
- Route every stdio response through the outbox so no runtime worker or module writes
  directly to the protocol descriptor.
- Add deterministic scheduling, coalescence, writer-failure, bounded-backpressure and
  multi-producer tests under native and sanitizer gates.

## 0.4.0 - 2026-08-14

- Add an independently enabled Resources module implementing `resources/list`,
  `resources/templates/list`, and `resources/read` for both supported MCP eras.
- Extend `maelys-provider/2` and the TypeScript/Python provider SDKs with static
  resources, URI templates, reads, and multi-round resource results.
- Introduce the opaque `maelys-uri` security facade over uriparser: exact-length
  input, RFC 3986 normalization, embedded/encoded NUL rejection, and bounded URI
  template validation.
- Validate text/blob exclusivity, MIME fields, base64 payloads, canonical URIs, and
  optional resource sizes before data crosses the provider boundary.
- Add native, process-provider, SDK, sanitizer, analyzer, and URI fuzz coverage.

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
