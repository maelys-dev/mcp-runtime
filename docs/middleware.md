# The middleware chain

`maelys_mcp_runtime_add_middleware` registers one link in the runtime's policy
and observation chain. The chain is the only place policy lives: it replaced
the runtime-wide `authorize`/`audit` callback pair that
`maelys_mcp_runtime_config_t` carried through ABI 2, and the two do not
coexist. `docs/middleware-design.md` is the design and the reasoning; this
document is the surface.

The design specifies seven hooks. This release implements the two decision
points — `on_authorize` (hook ②) and `on_audit` (hook ⑤) — plus the
registration, ordering and invocation machinery the remaining five plug into.

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

Registration order is observation order. `on_authorize` runs front to back and
**stops at the first middleware that does not allow**, so a later link can
never overturn an earlier deny. `on_audit` runs front to back and every hook
runs, whatever the outcome. `destroy` runs in reverse registration order at
`maelys_mcp_runtime_destroy`.

Registration is legal only while the runtime is cold — before its first
channel, exactly like `maelys_mcp_runtime_add_provider` — and returns
`MAELYS_MCP_ERR_STATE` afterwards. That immutability is what lets dispatch
read the chain with no lock, and it is why a runtime with no middleware costs
exactly what it cost before the chain existed: the per-hook counters are zero
and no hook context is built.

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

`outcome` is `MAELYS_MCP_OK` on success, `MAELYS_MCP_ERR_DENIED` when hook ②
denied, `MAELYS_MCP_ERR_STATE` when hook ② could not decide, and whatever the
provider or result validation produced otherwise. A denied `resources/read` is
journalled, which it was not before the chain.

## Threading and lifetime

Both hooks run on the thread dispatching one request, serialized within that
request, and may block. They may be entered concurrently for different
channels, so a middleware sharing mutable state across channels owns its own
locking. No runtime lock is held while a hook runs, and the documented lock
hierarchy is therefore not extended by the chain.

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
