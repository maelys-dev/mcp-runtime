# Middleware chain — design

> **Status: design, not implemented.** Nothing in this document describes
> current behaviour except the sections explicitly marked *today*. It is
> recorded here so the decisions and their reasons survive, and so the
> implementation can be reviewed against them rather than against memory.

## Purpose

Two goals drive this, and they are the same mechanism seen from two sides:

- **Transformation** — rename tools and arguments, hide arguments from clients
  while injecting values, alter schemas and descriptions, rewrite or redact
  results. Feature parity with FastMCP's tool transformation.
- **Proxy / MITM** — re-expose a third-party MCP server's tools through this
  runtime, applying effect classes, policy and audit to traffic the runtime
  did not originate.

What must **not** happen: a policy engine inside the runtime. No Datalog, no
`ALLOW`/`DENY`/`REQUIRE_CONFIRMATION` vocabulary, no roles, no per-user rules,
no business redaction. The runtime transports an opaque context to an adapter
and does not know what it means. `REQUIRE_CONFIRMATION` in particular belongs
to the orchestrator adapter, which suspends *before* calling
`maelys_mcp_channel_handle`; `input_required`/MRTR is a provider mechanism and
must not be repurposed to carry policy decisions.

## What exists today

Policy is a portal and a journal, not a pipeline:

```c
typedef struct maelys_mcp_request_context {
    const char *protocol_version;
    const char *client_name;
    const char *tool_name;
    const char *resource_uri;
    maelys_mcp_operation_t operation;
    maelys_mcp_tool_effect_t effect;
} maelys_mcp_request_context_t;
```

`authorize()` returns an int over that metadata alone — no arguments, no
result. `audit()` is void and fire-and-forget. **Five decision points**, one
callback each, no chain and no mutation: `tools.c:88` (list) and `:356`
(call), `resources.c:93` (list), `:127` (templates) and `:240` (read). `policy_context` is **runtime-global**;
`maelys_mcp_channel_config_t` carries only queue and timeout settings.

The response sink already exists and is in use, its first consumer being
`notifications/progress`:

```c
typedef struct maelys_mcp_response_sink {
    maelys_mcp_result_t (*emit)(void *context, json_t *message);
    maelys_mcp_result_t (*complete)(void *context, json_t *response);
    int (*cancelled)(void *context);
    void *context;
} maelys_mcp_response_sink_t;
```

**Today's order in `call_tool` is validation, then policy** — schema
validation at `tools.c:344`, `policy_allows` at `tools.c:356`.

## The chain

```
maelys_mcp_channel_handle(channel, request, sink)
  └─ runtime_dispatch → tools.c call_tool()
       ① on_resolve    client-facing name+args  →  real tool + real args
       ② on_authorize  decision, on the resolved identity
          schema validation
       ③ on_call       invoke or substitute; arguments are const
             └─► provider->call()
       ④ on_result     rewrite / redact
       ⑤ on_audit      observational, fire-and-forget
  └─ sink->complete(response)
       ⑥ wrap_sink     decorates the sink

tools/list: list_tools() → policy(LIST) → ⑦ on_list → serialisation
```

| Hook | Sees | For |
|---|---|---|
| ① `on_resolve` | client name + arguments | rename, hide/inject arguments, map to the real tool |
| ② `on_authorize` | resolved identity, effect, channel context | adapter decision, deny |
| ③ `on_call` | the provider request, **const** | proxy, cache, short-circuit |
| ④ `on_result` | the provider result | redaction, output rewriting |
| ⑤ `on_audit` | both views, plus outcome | journal |
| ⑥ `wrap_sink` | every outbound frame of the request | progress coalescing, stream redaction |
| ⑦ `on_list` | the catalog | the other half of transformation |

A middleware implements only the hooks it needs: a transform takes ①④⑦, a
proxy ③, a redactor ④⑥.

**This reorders policy ahead of schema validation, which is a behaviour
change**, not the status quo. It is wanted — a denied caller should not be
able to probe argument schemas through validation error details — but it must
ship as a named change, not slip in as a side effect.

## Checked against real transformation use cases

The hook set was validated against FastMCP's own worked examples, not only
against this codebase. Four of five map cleanly: renaming arguments is ⑦ plus
①; a hidden argument with a constant default is ⑦ removing it from the
published schema and ① injecting it; a `default_factory` is the same with ①
computing per call; a per-user tool is ⑦ filtering the catalog per channel and
① injecting from that channel's context.

That last one is worth noting: FastMCP builds one tool object per user at
registration, which suits one server per user. It is **unimplementable in a
multi-client runtime without the per-channel context** — the strongest
justification that decision has. It also confirms the audit amendment, since
FastMCP's canonical hidden-argument example is literally an `api_key`: an
audit seeing only post-injection state would log it.

Two precisions the examples force, neither of which was stated above:

- **① must emit arguments valid against the *real* schema.** Validation runs
  after ① and checks the underlying tool's schema, not the published one. A
  transform that changes an argument's *type* must therefore convert in ①;
  rewriting the type in ⑦ alone makes validation reject every call.
- **The `forward()` pattern survives but splits in two.** FastMCP expresses
  "validate, maybe raise, then call the original with remapped arguments" as
  one function. Here it is ① for the remapping and ③ for the short circuit.
  That is the price of ③'s const arguments, and it is a real ergonomic cost,
  not a wash.

## Why hooks rather than one `call_next`

An onion wrapper subsumes the flat hooks: it can rewrite arguments before
calling the next layer and rewrite results after. That makes ① and ④
redundant and, worse, defeats the ordering invariant — arguments mutated
inside an onion are mutated **after** schema validation.

So ③ is deliberately constrained to *invoke or substitute*, with const
arguments. Argument rewriting stays ①'s exclusive job, result rewriting ④'s.
This loses one FastMCP pattern (retry-with-different-arguments inside a
wrapper). That is the intended trade: the hooks are not expressiveness, they
are **pinning points** where the runtime enforces an order no middleware can
bypass.

## Ordering: policy sees the resolved identity

Transformation resolves *which real tool and which real arguments*, then
policy decides on the result. A rename must not route around a deny rule —
`hermes.content.apply` re-exposed as `edit_doc` must still be seen as an
apply. Transformation is presentation, never privilege escalation.

Three amendments to that rule:

- **Audit sees both views.** The journal needs what the client asked for and
  what executed; the difference is where intent lives. If ⑤ saw only the
  post-injection state it would also **log injected secrets**, so the client's
  view is the one recorded and the injected delta is treated as sensitive.
- **A proxied upstream is not a trust anchor.** Its tool names are
  remote-controlled and can change between `list` and `call`. Upstream
  identity must be pinned as `(upstream-id, name)` snapshotted at list time
  and calls resolved against that snapshot, or the deny rule has a TOCTOU.
- **Synthetic tools have no registry entry.** Tools that ⑦ adds — proxy tools,
  code-mode meta-tools — never pass `policy(LIST)` and resolve to nothing
  registry-backed. ② therefore operates on the *resolved* identity, which may
  be virtual; the invariant is "post-resolution", not "registry entry".

## State and lifetime

The chain is registered before `serve` and immutable thereafter, so the hot
path needs no locking. Per-channel state is one **opaque slot bound at channel
creation** and passed to every hook, in **both directions**. That slot is the
per-principal, per-session mechanism: the runtime carries the pointer and never
interprets it. It is read-only from the runtime's side; an adapter needing
mutability owns its own locking behind that pointer.

The runtime has no write API because it has nothing of its own to write:
modern-protocol identity is client-asserted per request and defaults to
unknown, so the embedder-bound pointer is the only anchor worth
authenticating against — middleware must never base a decision on
`client_name`. Two obligations follow, neither checkable by the runtime: the
box is freed only after `channel_destroy` returns, and one channel maps to one
principal. One API gap follows too: `serve_stdio` creates its channel
internally, so there is currently no seam to bind the context when using the
stock transport.

Without it no chain can distinguish two clients sharing one runtime, however
many hooks it has. It is a prerequisite, not a later refinement.

## Both directions, and why they cannot be one interface

The chain is bidirectional. The inbound half is ①–⑤; the outbound half is ⑥
plus a hook on **fanout**, which is the genuinely reverse path.

It is worth being precise about how much reverse traffic actually exists here,
because it is less than a FastMCP comparison suggests. In FastMCP, sampling and
elicitation are separate server→client requests needing their own forwarding
path. In `2026-07-28` they travel *inside* the result as `inputRequests`, so
they already pass through ④ and the sink. The resumable MRTR pattern this
runtime kept is what makes that true.

What remains genuinely reverse:

- **Subscription fanout**, which bypasses the sink entirely today
  (`notify_resource_updated` snapshots the channel list and enqueues straight
  into each channel's outbox). This is where a MITM filtering *which resources
  a principal may even know changed* has to live.
- **A proxied legacy upstream.** A `2025-11-25` upstream may send real nested
  server→client requests. Translating them into the resumable shape is the
  proxy provider's job, but a middleware may legitimately want to see them.

**The two directions have different threading contracts, and conflating them
would be a trap.** Inbound hooks run on the thread handling one request,
serialized within that request, and may block. A fanout hook must be
**non-blocking and reentrant across channels**, for two distinct reasons:

- *Reentrant*, because `notify_*` may be called concurrently by several
  producers, so the same hook is entered concurrently for different channels.
- *Reentrant for the **same** channel too*, not merely across channels:
  concurrent producers — the provider event thread plus any embedder thread
  calling `notify_*` — each walk the full channel list.
- *Non-blocking*, because `emit_event` walks the targeted channels
  **sequentially**.

Be precise about what that second point protects, because the obvious
justification is wrong twice over. It is not lock contention: the channel
mutex is released before the message is built and enqueued
(`emit_event_to_channel`). Nor is it a no-stall invariant — **there is no such
invariant today**. `maelys_mcp_outbox_enqueue_take` waits for admission up to
`admission_timeout_ms`, 5000 ms by default, inside that sequential walk, so a
congested channel already delays every peer after it by up to five seconds per
event. `docs/subscriptions.md` claims only that channel *close* never stops
fanout to peers, which is a different guarantee.

The contract therefore exists to avoid **converting a bounded stall into an
unbounded one**, not to preserve a property the runtime does not have.

Fanout also enqueues with `fault_on_timeout` cleared, unlike the request path —
a slow consumer does not fault its channel here — and fanout messages coalesce
by key, so a later event can replace one already queued. A fanout hook
therefore cannot serve as a delivery audit: "the hook approved it" does not
mean "the client received it".

They therefore get separate types with separate documented contracts, not one
interface used in two places. `wrap_sink` covers per-request outbound frames;
fanout is scoped to a channel and a subscription, not to a request, and cannot
borrow the request-scoped contract.

## Decisions taken

- **The chain replaces `authorize`/`audit`; it does not coexist with them.**
  Two similar-looking policy mechanisms double the audit surface on the
  security-critical path, which is the worst place to spend this project's
  budget. Coexistence also forces an unanswerable ordering question: legacy
  `authorize` placed before ① sees client-facing names a rename routes around,
  placed after it silently changes meaning for adapters written against
  client-facing names. A provided compat middleware implementing only ② and ⑤
  reproduces today's behaviour, so migration is mechanical.
- Registering "just an authorizer" must stay as ergonomic as today's two
  function pointers. If the degenerate case gets harder, the replacement was
  botched. Note the compat middleware reproduces today's *decision surface*,
  not today's observable ordering — the policy/validation reorder above is a
  deliberate, named change.

## Known holes, named rather than hidden

- **Subscription fanout bypasses the sink**, so ⑥ does not cover
  `resources/updated`. This is now in scope rather than a hole — see "Both
  directions" above — but it is the piece with the hardest contract, because
  it runs concurrently on a producer thread rather than on a request thread.
- **Nothing ties `tools/list_changed` to middleware state.** The trigger
  itself exists (`maelys_mcp_runtime_notify_tools_list_changed`, fired by
  provider events); what is missing is any way for a change in ⑦'s output, or
  in an upstream's live catalog, to fire it.
- **Result-schema validation versus ④.** `validate_provider_result` runs on the
  provider's result. A ④ that rewrites afterwards yields a result checked
  against no schema — or, if ⑦ rewrote the advertised `outputSchema`, against
  the wrong one. The runtime validates the real result against the real schema;
  keeping ⑦ and ④ consistent is the middleware author's responsibility and the
  runtime cannot check it.
- **The resource path needs the same treatment, and this is a hard sequencing
  constraint, not a nicety.** *Three of the five* policy decision points are in
  `resources.c`. Because the chain **replaces** `authorize`/`audit`, shipping
  it before resource hooks exist would silently remove resource authorization
  altogether — the compat middleware would have nowhere to hang it. Resource
  hooks must land in the same release as the replacement.
- **MRTR continuation traffic is invisible to every hook.** `requestState` is
  an opaque provider blob round-tripped through the client, and
  `inputResponses` carries elicitation answers and sampling completions — the
  most sensitive inbound payloads in the protocol. Both are siblings of
  `arguments`, and ① as specified sees only name and arguments. A continuation
  can also arrive on a *different* channel than the one that issued it, so the
  per-channel context cannot fence a stolen or stale `requestState`: the
  "swap anonymous for authenticated" scenario leaves pre-auth continuations
  valid after the swap. ① and ② must see the full call params, or a
  continuation hook must exist. This is the largest genuine gap in the design.
- **`server/discover` and `initialize` sit outside every hook and every policy
  point.** Consistent with today, but a MITM claiming to govern what a
  principal may know should name it.
- **Close-time subscription completions bypass both directions.**
  `maelys_mcp_channel_complete_subscriptions_until` enqueues responses
  directly, and the originating request's sink is long gone.

## Out of scope, and why

**Code mode is a provider, not middleware.** It replaces the catalog with
meta-tools and executes LLM-written code in a sandbox; a sandbox for untrusted
code does not belong in a C runtime whose value is an auditable surface. The
catalog half is just ⑦, and the sandbox lives in a provider.

One part of it *is* the runtime's problem and must be decided before it is
ever attempted: the sandboxed script's inner `call_tool` has to re-enter
dispatch **through the chain, on the same channel and principal**, or code mode
is a policy bypass — the script calls tools its client was denied. That
re-entry collides with two verified constraints: provider calls are strictly
single-outstanding per process (`exchange_mutex` held for the whole round
trip, so a script calling a tool on its own provider deadlocks), and the
documented lock hierarchy has no reentrant path. Retrofitting reentrancy into
a lock hierarchy is precisely the surgery this codebase is built to avoid.

The reverse direction is **in scope** — see "Both directions" above for what it
actually amounts to, and why its threading contract differs from the inbound
half.

## C-specific contracts to settle before writing code

- `json_t` refcount ownership across hooks — borrowed in, owned-or-NULL out.
  This is where leaks and double frees will actually live.
- An error taxonomy: deny, internal error and short-circuit must map to
  distinct JSON-RPC outcomes.
- Which thread each hook runs on, relative to the lock hierarchy.
- `⑥` needs a contract: `complete` forwarded exactly once (a middleware that
  swallows it wedges the request, and the runtime should detect that),
  `cancelled` passed through by default, wrapping order the reverse of ④'s.
