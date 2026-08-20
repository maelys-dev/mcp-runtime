# The middleware chain

`maelys_mcp_runtime_add_middleware` registers one link in the runtime's policy
and observation chain. The chain is the only place policy lives: it replaced
the runtime-wide `authorize`/`audit` callback pair that
`maelys_mcp_runtime_config_t` carried through ABI 2, and the two do not
coexist. `docs/middleware-design.md` is the design and the reasoning; this
document is the surface.

All seven hooks of the design are implemented: the two decision points —
`on_authorize` (②) and `on_audit` (⑤) — and the five transformation hooks —
`on_resolve` (①), `on_call` (③), `on_result` (④), `wrap_sink` (⑥) and
`on_list` (⑦).

| Hook | Sees | For |
|---|---|---|
| ① `on_resolve` | the client's tool name and arguments | rename, hide or inject arguments, map onto the real tool |
| ② `on_authorize` | the resolved identity, effect, channel, params | the decision |
| ③ `on_call` | the resolved call, **read-only** | proxy, cache, short-circuit |
| ④ `on_result` | the provider's result | redaction, truncation, output rewriting |
| ⑤ `on_audit` | both views, plus the outcome | the journal |
| ⑥ `wrap_sink` | every outbound frame of one request | progress coalescing, stream redaction |
| ⑦ `on_list` | a catalog, after ② filtered it | the other half of transformation |

A middleware implements only what it needs: a transform is ①④⑦, a proxy ③, a
redactor ④⑥, an authorizer ②.

```c
static maelys_mcp_authorize_decision_t decide(
    void *context,
    const maelys_mcp_authorize_context_t *request) {
    const principal_t *principal = maelys_mcp_channel_context(request->channel);
    if (!principal) return MAELYS_MCP_AUTHORIZE_DENY;
    return may_use(principal, request->tool_name, request->effect) ?
        MAELYS_MCP_AUTHORIZE_ALLOW : MAELYS_MCP_AUTHORIZE_DENY;
}

maelys_mcp_middleware_t policy = {
    .name = "acme-policy",
    .context = &adapter,
    .on_authorize = decide
};
maelys_mcp_runtime_add_middleware(runtime, &policy, &error);
```

## The five decision points

Every policy decision this runtime takes reaches hook ②, and each carries the
resolved identity for its operation:

| `operation` | Asked once per | `tool_name` / `resource_uri` | A deny means |
|---|---|---|---|
| `MAELYS_MCP_OPERATION_LIST` | tool in `tools/list` | the tool's registered name | the tool is omitted from the catalog |
| `MAELYS_MCP_OPERATION_CALL` | `tools/call` | the resolved tool | `-32003`, before the provider is reached |
| `MAELYS_MCP_OPERATION_RESOURCE_LIST` | resource in `resources/list` | the registered, normalized URI | the resource is omitted |
| `MAELYS_MCP_OPERATION_RESOURCE_TEMPLATE_LIST` | template in `resources/templates/list` | the URI template | the template is omitted |
| `MAELYS_MCP_OPERATION_RESOURCE_READ` | `resources/read` | the **canonical** URI, after normalization | `-32003`, before any provider is consulted |

`server/discover`, `initialize` and the subscription methods sit outside the
chain, as they sat outside the callbacks before it.

## Three outcomes, not two

`MAELYS_MCP_AUTHORIZE_ALLOW` is the only value that lets an operation proceed.
Anything else denies, including a value a later release adds — a middleware
returning a zeroed decision fails closed, which is why `DENY` is the zero.

`MAELYS_MCP_AUTHORIZE_ERROR` is not a denial. It says the adapter could not
reach a verdict, which is a fault in the policy layer rather than a statement
about the caller, and it maps to `-32603` with the message
`Policy evaluation failed`. On a listing it fails the whole request instead of
dropping the entry: a hidden catalog entry and an unevaluated one look
identical to a client, so only a real deny may omit one.

## Ordering

Registration order is observation order.

- `on_authorize` runs front to back and **stops at the first middleware that
  does not allow**, so a later link can never overturn an earlier deny.
- `on_call` runs front to back and **stops at the first substitution**, so a
  cache in front of a proxy stops the proxy running.
- `on_resolve`, `on_result` and `on_list` run front to back and **every hook
  runs, chained** — each sees the previous one's output, so two independent
  transforms compose rather than conflict.
- `on_audit` runs front to back and every hook runs, whatever the outcome.
- `wrap_sink` is the one reversal. The first-registered middleware wraps
  innermost, so it is the **last** to touch an outbound frame before the
  transport — the mirror of ④'s order, and the position "closest to the
  client" in an onion.
- `destroy` runs in reverse registration order at
  `maelys_mcp_runtime_destroy`.

Registration is legal only while the runtime is cold — before its first
channel, exactly like `maelys_mcp_runtime_add_provider` — and returns
`MAELYS_MCP_ERR_STATE` afterwards. That immutability is what lets dispatch
read the chain with no lock, and it is why a runtime with no middleware costs
exactly what it cost before the chain existed: the per-hook counters are zero
and no hook context is built.

## What hook ① resolves

`on_resolve` is the request-side transformation point, and the only one. It
turns the name and arguments the client wrote into what the runtime will
actually run, on `tools/call`. Everything downstream sees the resolved form.

```c
static maelys_mcp_result_t resolve(
    void *context,
    const maelys_mcp_resolve_context_t *request,
    maelys_mcp_resolution_t *out_resolution) {
    if (strcmp(request->tool_name, "edit_doc") != 0) return MAELYS_MCP_OK;
    json_t *arguments = json_deep_copy(request->arguments);
    out_resolution->tool_name = strdup("hermes.content.apply");
    out_resolution->arguments = arguments;   /* both now owned by the runtime */
    return MAELYS_MCP_OK;
}
```

Three rules, and each of them is a place a transform goes quietly wrong.

- **Arguments are validated against the *real* tool's schema**, after this
  hook. A transform that changes an argument's **type** must therefore convert
  it here; rewriting the type in ⑦ alone republishes a lie and every call
  fails validation.
- **The registry is not consulted on the client's spelling.** A name ⑦
  published, or one a proxy invented, has no registry entry, so resolution
  runs first and `Unknown tool` is reported against the *resolved* name.
- **The audit keeps the client's view.** Hook ⑤ receives the request's params,
  which never carried what ① injected — the canonical injected value is an API
  key, and an audit built on the resolved arguments would write it to a log
  file. That is why `maelys_mcp_audit_context_t` has no resolved-arguments
  field, and it is a deliberate limit, not an omission.

Leave a field NULL for "unchanged": an all-NULL resolution allocates and
copies nothing. A non-NULL field transfers ownership — `tool_name` is released
with `free()`, so it must come from `strdup`, not from a literal. A hook that
returns anything but `MAELYS_MCP_OK`, or an `arguments` that is not an object,
fails the request with `-32603` and `Request transformation failed`; the
provider is never reached and the failure is journalled with the client's
name, never as a denial.

`resources/read` has no ① yet. Resource identity resolves through URI
normalization, which ② and ⑤ already see both sides of.

## What hook ③ may do

`on_call` invokes or substitutes, and nothing else — its arguments are
read-only. That constraint is the point of flat hooks rather than one onion
wrapper: arguments mutated inside a wrapper would be mutated *after* schema
validation, defeating the ordering the chain exists to pin.

The price is real and worth naming. FastMCP's `forward()` pattern — "validate,
maybe raise, then call the original with remapped arguments" — is one function
there and splits in two here: ① for the remapping, ③ for the short circuit.

A substituting hook fills a `maelys_mcp_provider_result_t` exactly as a
provider's `call()` does, and the result faces the same validation a
provider's would, **output schema included**: something answering in the
provider's name answers to the provider's contract. `MAELYS_MCP_CALL_ERROR`
maps to `-32603` carrying the hook's own message. A hook that wants to report
a *tool* failure rather than a chain failure does it the way a provider does,
with `is_error` on the result it returns.

③ receives no progress reporter: a substitution answers immediately by
definition, and the reporter is bound to the provider callback. A middleware
that needs to emit request-scoped frames wraps the sink instead (⑥).

## What hook ④ may rewrite, and what it may not

`on_result` is where redaction, truncation and output rewriting live. Its
placement relative to validation is a decision:

1. the runtime validates the **provider's** result against the **real** output
   schema — a provider that breaks its own declared contract is caught
   whatever the chain does, and ④ is not even consulted;
2. ④ then runs, chained, each hook seeing the previous replacement;
3. a replacement is re-checked **structurally** — it must still be a result the
   runtime can serialize, and its content blocks still face the wire checks —
   but **not** against the tool's `outputSchema`.

That last exclusion is deliberate: emitting a value the published schema no
longer describes is what redaction *is*. The consequence is the design's named
hole, and it stays named — **keeping ⑦'s advertised `outputSchema` consistent
with what ④ emits is the middleware author's responsibility.** The real schema
is passed to the hook (`output_schema`) so it can see what it would have to
satisfy; the runtime does not check that it did.

④ runs only on a successful, validated result. A failed provider call has no
result object to rewrite, and its error text reaches the client unchanged.

## What hook ⑥ wraps

`wrap_sink` decorates the path one request's outbound frames leave by: the
request-scoped notifications it emits — `notifications/progress` today — and
then its single final response.

The sink stays opaque. A wrapper never builds one; it fills a
`maelys_mcp_sink_wrapper_t` and forwards to the inner sink through
`maelys_mcp_sink_emit`, `maelys_mcp_sink_complete` and
`maelys_mcp_sink_cancelled`. **Every member is optional**: a NULL one forwards
untouched, so "coalesce progress and leave everything else alone" is one
function pointer. Allocate per-request state in `wrap_sink`, put it in
`context`, and free it in `release`, which runs exactly once per request
whatever the outcome.

**Ordering survives wrapping, and does not depend on the wrapper.** Progress
frames are delivered strictly ahead of the response they belong to — over SSE
the response terminates the stream, so anything behind it is not late, it is
lost. The runtime guarantees this by construction: frames are forwarded as
they are produced, and `complete` is never called until dispatch has returned.
A wrapper preserves it simply by forwarding in the order it is called. A
wrapper that **buffers** emitted frames must flush them before forwarding
`complete`.

Two obligations the runtime enforces rather than trusts:

- **`complete` reaches the transport exactly once.** A wrapper that accepts
  the response and never passes it on would wedge that request id for the life
  of the connection; the runtime detects that and answers past the chain with
  `-32603` and `Response was not delivered`. A second forwarded completion is
  refused with `MAELYS_MCP_ERR_STATE`, because two responses for one id is a
  protocol violation whichever one a client believes.
- **A failing `wrap_sink` still answers.** The request is not dispatched and
  the client gets `-32603` with `Response sink wrapping failed`; wrappers built
  before the failure are released.

Subscription fanout (`notifications/resources/updated`) does **not** pass
through ⑥ — it bypasses the sink entirely and runs on a producer thread with a
different threading contract. That remains the design's named hole.

## What hook ⑦ publishes

`on_list` transforms `tools/list`, `resources/list` and
`resources/templates/list`; `catalog` says which. It receives the entries that
survived ②, as the JSON array they would be serialized as, and returns either
NULL for "unchanged" — the zero-copy path — or a new array the runtime takes
over.

It runs **after** ②, so a denied entry is already gone and cannot be
transformed back into view. An entry it invents has no registry backing, which
is what a proxy or a retrieval-first meta-tool needs — and a call to that name
still passes ① and ②, so a synthetic name is not a way around a decision.

A hook that fails, or returns something that is not an array, fails the whole
listing with `-32603` and `Catalog transformation failed` — for the same
reason an undecidable ② does: a catalog that could not be transformed is not a
shorter catalog.

Nothing yet ties a change in ⑦'s output to `tools/list_changed`. The trigger
exists; the link does not.

## What hook ② sees

- **The resolved identity, never the client's spelling.** Transformation is
  presentation, not privilege: once hook ① exists, a rename must resolve
  before the decision, so re-exposing `hermes.content.apply` as `edit_doc`
  cannot route around a rule about applies. The invariant is
  "post-resolution", not "registry-backed" — a synthetic tool with no registry
  entry still arrives here.
- **The channel**, for `maelys_mcp_channel_context()`. That embedder-bound
  pointer is the per-principal anchor, and the only thing worth
  authenticating against: `client_name` is client-asserted, unauthenticated
  and defaults to `unknown` in both protocol eras. Record it; never decide on
  it.
- **The request's whole `params`.** `inputResponses` and `requestState` are
  siblings of `arguments`, so a hook seeing only a name and its arguments
  would be blind to every MRTR continuation — elicitation answers and sampling
  completions included. Two properties matter: those params are **not yet
  schema-validated** (see below), so treat them as untrusted; and they are
  read-only, because rewriting arguments is hook ①'s job.

Visibility is not binding. A continuation may still arrive on a different
channel than the one that issued it, so the per-channel context cannot by
itself fence a stale or stolen `requestState`.

## Policy runs before schema validation

On `tools/call` the decision is taken **before** arguments are validated. This
is a deliberate change from the pre-chain runtime: a caller who may not use a
tool should not be able to map its argument schema by reading the `detail`
field of a validation error. A denied call therefore returns `-32003` with no
data, whether or not its arguments would have validated.

## What hook ⑤ records

`on_audit` is observational and fire-and-forget: `void` return, no influence
on the outcome, every registered hook runs. It fires on `tools/call` and
`resources/read` — the two operations that execute something — and not on
listings, which is what the callbacks did before it.

It carries **both views**. `requested_tool_name`/`requested_resource_uri` are
what the client asked for; `tool_name`/`resource_uri` are what executed. The
difference between them is where intent lives, and an audit built on the
resolved view alone would eventually log values a transform injected — which
is how a hidden credential reaches a log file. Record the requested view. On
the resource path the two already differ today, since URI normalization
rewrites what the client sent.

On `tools/call` the two views genuinely differ as soon as hook ① renames
something; on the resource path they already differ today, since URI
normalization rewrites what the client sent.

`outcome` is `MAELYS_MCP_OK` on success, `MAELYS_MCP_ERR_DENIED` when hook ②
denied, `MAELYS_MCP_ERR_STATE` when a hook could not decide or a
transformation hook failed, and whatever the provider or result validation
produced otherwise. A denied `resources/read` is journalled, which it was not
before the chain.

## Ownership across the hooks

One rule, everywhere: **borrowed in, owned-or-NULL out**.

| What | Direction | Rule |
|---|---|---|
| `params`, `arguments`, `entries`, `result` on any context | in | borrowed for the call, read-only, never retained |
| `maelys_mcp_resolution_t.tool_name` | out | NULL means unchanged; otherwise the runtime `free()`s it |
| `maelys_mcp_resolution_t.arguments` | out | NULL means unchanged; otherwise one reference transfers |
| `out_entries` from ⑦ | out | NULL means unchanged; otherwise one reference transfers |
| `out_result` from ③ and ④ | out | filled only when substituting or replacing; the runtime releases every field |
| `out_error` from ③ and ④ | out | `malloc`ed; the runtime frees it |
| a message passed to `maelys_mcp_sink_emit`/`_complete` | through | stolen on success, left with the caller on failure |

NULL is always the zero-copy answer, which is what keeps a chain that observes
without transforming as cheap as no chain at all.

## Threading and lifetime

Every hook — the five inbound ones and the three functions of a ⑥ wrapper —
runs on the thread dispatching one request, serialized within that request,
and may block. They may be entered concurrently for **different requests**,
including two requests on the *same* channel: `tools/call` and
`resources/read` dispatch on their own worker threads so that a call can block
on a nested client round trip without stalling the connection (see
`docs/architecture.md`). A middleware sharing mutable state across requests
therefore owns its own locking — the "different channels" caveat this
paragraph used to make was never the whole of it, and is now visibly not. No
runtime lock is held while a hook runs, and the documented lock hierarchy in
`src/internal/internal.h` is therefore not extended by the chain, ⑥ included:
a wrapper's per-request state lives on the dispatching thread's stack frame
and is never shared with another request or another channel.

A dispatched request's JSON — the object every hook's `params` points into — is
private to the thread dispatching it, so a hook may read it without
coordinating with any other request.

`name` and `context` are borrowed, not copied, and must outlive the runtime.
The descriptor itself is copied, so it may live on the caller's stack. If
`destroy` is set it is called once, with that context, from
`maelys_mcp_runtime_destroy` — after the last channel is gone and provider
events have stopped, so no hook can still be running. Registration never takes
ownership on failure.

## Migrating from `authorize`/`audit`

`maelys_mcp_runtime_config_t` no longer has `authorize`, `audit` or
`policy_context`. One call takes the same three values:

```c
maelys_mcp_runtime_add_compat_policy(runtime, authorize, audit, &policy);
```

The compatibility middleware implements ② and ⑤ and calls the old callbacks
with exactly the metadata they used to receive. What it cannot forward is what
the old signature has no field for: the channel and the request params. It
reproduces the old **decision surface**, not the old observable ordering —
policy now precedes validation, and a denied read is now audited. Both are
named changes, not regressions to work around.
