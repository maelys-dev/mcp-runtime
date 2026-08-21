# Authenticated transport principal — design

> **Status: the runtime half is shipped; the transport half is not.** The
> ABI 4 channel config with `context_release`/`release_context` — the
> mechanism this contract's principal lifetime rests on — shipped in 0.18.0,
> and the authenticator seam with the `loopback-trust` and `static-bearer`
> reference authenticators landed with the H1 server layer. What does NOT
> exist yet is the consumption: no channel is created from an HTTP request
> and no principal is bound to one until the transport's dispatch phase
> lands. This document is the
> contract `docs/http-transport-design.md` consumes; it is deliberately
> separate, and reviewable alone, because it is not about HTTP. The principal
> is a property of *a transport that has an authentication boundary*, and a
> design that buries it inside the HTTP listener would have to be rewritten the
> first time a unix socket or an mTLS listener wanted the same thing.
> **Accepting the identity seam must not require accepting an HTTP parser**,
> which is why this is its own change: the transport document depends on this
> one, not the reverse.
>
> Two things here are **API changes to a released library**, not new code
> beside it: `maelys_mcp_channel_config_t` gains three fields and
> `maelys_mcp_channel_set_protocol_eras` is removed, taking
> `MAELYS_MCP_ABI_VERSION` from 3 to 4. See
> [Channel-context ownership](#channel-context-ownership-and-abi-4). That is
> the part to review hardest; everything else is additive.
>
> Verified against `ca85409` (release 0.17.0). Every code claim below carries a
> `file:line` citation, and every citation was re-read against that commit
> rather than recalled or carried over from an earlier base.

## Why this is a prerequisite and not a refinement

`roadmaps/P1-RUNTIME-TRANSPORT-ROADMAP.md` in `mcp-runtime-engineering`
already settled the ordering, and it is worth quoting the constraint rather
than restating it: the "Authenticated transport principal" branch is listed
with `Dependency: accepted P1-C01`, the "Native HTTP POST and request-scoped
SSE" branch with `Dependencies: accepted P1-C01 **and accepted
authenticated-principal cycle**`, and the roadmap's closing section says HTTP
"must not proceed before authenticated principals and per-principal effect
policy are defined". This document exists to make that dependency dischargeable
rather than perpetual.

The runtime's own security model says the same thing in its own words, and as
of `475333b` it says it while pointing at this document's subject:

> The runtime remains a local stdio host. It does not expose HTTP, authenticate
> a network principal or apply per-principal effect policy. Those controls are
> mandatory before a network transport can safely expose providers that run
> with host privileges; the middleware chain's per-channel context and
> resolved-identity policy decisions are the foundation a transport-established
> principal will bind to.
>
> — `docs/security-model.md:82`

Nothing in that paragraph has been discharged. Its final clause is the
commitment this design has to honour: the per-channel context is already the
intended anchor, so what follows must *fill* that slot rather than invent a
second identity mechanism beside it.

## What already exists, and what it is missing

The anchor is already in the public ABI. `maelys_mcp_channel_config_t` carries
one opaque, embedder-owned pointer, and its contract is written into the header
(`include/maelys/mcp/channel.h:21`):

```c
typedef struct maelys_mcp_channel_config {
    size_t max_messages;
    size_t max_bytes;
    size_t response_burst;
    unsigned int admission_timeout_ms;
    unsigned int close_timeout_ms;
    void *context;      /* opaque, embedder-owned */
} maelys_mcp_channel_config_t;
```

Three properties of that slot are already load-bearing and must not change:

- It is bound **at channel creation** and copied by value into the channel
  (`src/core/channel.c:17`), never afterwards. There is no write API.
- It is read back through `maelys_mcp_channel_context()`
  (`src/core/channel.c:775`), which is what the middleware chain's
  `on_authorize` hook uses to distinguish two clients sharing one runtime.
- The header states the two obligations the runtime cannot check: the embedder
  must not free what the pointer designates until
  `maelys_mcp_channel_destroy` has returned, and **one channel maps to one
  principal** (`include/maelys/mcp/channel.h:26`).

What is missing is not the slot. It is two things around it.

**Nothing fills it from an authentication boundary.** No transport has a seam
for doing so, and the one transport that exists takes a whole
`maelys_mcp_channel_config_t` from its caller
(`maelys_mcp_stdio_options_t.channel_config`, used at
`src/transport/stdio.c:184`) without any opinion about what the `context`
inside it means. That is what [The seam](#the-seam) below supplies.

**And the slot has no destructor**, which 0.17.0 turned from a wart into a
blocker. `maelys_mcp_channel_destroy_detached` shipped in that release
(`include/maelys/mcp/channel.h:165`) and is what makes a channel-per-request
transport viable — but its contract is explicit about the cost:

> The channel's context (`maelys_mcp_channel_config_t::context`) may therefore
> outlive this call. An embedder that frees what it designates must do so from
> the runtime it detached the channel into, after `maelys_mcp_runtime_destroy`
> — which waits for every detached channel — and not on the return from this
> call.
>
> — `include/maelys/mcp/channel.h:155`

For a stdio host that destroys one channel at process exit, "free it after
`runtime_destroy`" is free. **For a long-lived HTTP server it is a leak with a
schedule**: every detached channel's principal reference stays held until the
whole process shuts down, so a server that detaches once an hour accumulates
principals for its entire uptime. An earlier draft of this document promised
that "the last worker releases the principal", and that promise is **not
implementable against the shipped 0.17.0 API** — there is no callback for the
last worker to invoke. Closing that is
[Channel-context ownership](#channel-context-ownership-and-abi-4), and it is
the reason this document is an API change rather than a convention.

Note precisely what "one channel maps to one principal" buys and what it does
not. It is an *identity* binding, not a *freshness* binding: it says every
request on this channel belongs to this principal, and says nothing about a
request whose payload was minted under a different one. That distinction is the
whole of the MRTR continuation hole in `docs/middleware-design.md:407`, and this
design does not close it — see "Not in v1".

## The seam

One function, one lifetime pair, one type the runtime never looks inside.

```c
/* Opaque to the runtime and to every transport. The authenticator's own type. */
typedef struct maelys_mcp_principal maelys_mcp_principal_t;

typedef struct maelys_mcp_authenticator {
    const char *name;
    void *context;

    /*
     * Called once per inbound transport unit, before any channel exists.
     * `credentials` is a transport-supplied, read-only view of the material
     * the transport actually authenticated against - never of the payload.
     * On MAELYS_MCP_OK, *out_principal is a retained reference the caller
     * releases exactly once.
     *
     * MAELYS_MCP_ERR_DENIED  - the credential was present and rejected.
     * MAELYS_MCP_ERR_ARGUMENT- the credential was absent or malformed.
     * anything else          - the authenticator could not decide.
     *
     * The three are distinct because a transport must map them to three
     * different answers, and conflating "rejected" with "could not decide"
     * is how an outage becomes an authorization bypass.
     */
    maelys_mcp_result_t (*authenticate)(
        void *context,
        const maelys_mcp_transport_credentials_t *credentials,
        maelys_mcp_principal_t **out_principal);

    /* Refcount operations on the authenticator's own type. Both are
     * thread-safe; the runtime calls neither. */
    void (*retain)(void *context, maelys_mcp_principal_t *principal);
    void (*release)(void *context, maelys_mcp_principal_t *principal);

    /* Called once from the transport's teardown, after the last channel that
     * could have carried one of its principals is destroyed. */
    void (*destroy)(void *context);
} maelys_mcp_authenticator_t;
```

`maelys_mcp_transport_credentials_t` is the second half of the contract and is
the part that makes the rule enforceable rather than merely stated. It is a
tagged view over *transport-established* facts only, and it is deliberately
buildable from **headers alone**, before any request body has been read — see
"Authentication runs before the body" below, which is what forces that
property:

```c
typedef enum maelys_mcp_credential_kind {
    MAELYS_MCP_CREDENTIAL_NONE = 0,       /* loopback trust, no material */
    MAELYS_MCP_CREDENTIAL_BEARER = 1,     /* an Authorization: Bearer value */
    MAELYS_MCP_CREDENTIAL_PEER = 2,       /* unix-socket peer credentials */
    MAELYS_MCP_CREDENTIAL_TLS_CLIENT = 3  /* a verified client certificate */
} maelys_mcp_credential_kind_t;

typedef struct maelys_mcp_transport_credentials {
    maelys_mcp_credential_kind_t kind;
    /* The peer address the transport observed, never a header. */
    const char *peer;
    /* Length-explicit, because a credential is not a C string. */
    const void *material;
    size_t material_length;
} maelys_mcp_transport_credentials_t;
```

There is deliberately no `headers` member and no `json_t *`. An authenticator
that wanted to read `clientInfo.name` would have to be handed it, and it is not
handed it. That is the point: the prohibition below is expressed in the type
signature, not only in prose.

## The hard rule

**The principal is established by the transport at channel creation, from a
real authentication boundary, and never from anything the payload carries.**

Four sources are legitimate:

- a bearer token the transport itself validated;
- a TLS client certificate the transport itself verified;
- unix-socket peer credentials the kernel supplied (`SO_PEERCRED` /
  `LOCAL_PEERCRED`);
- an explicitly configured local identity — the loopback-trust case, where the
  principal *is* "whoever can reach this socket", stated as such rather than
  inferred.

Everything else is forbidden as a source, and the list is not decorative:

- `params._meta["io.modelcontextprotocol/clientInfo"]` and the
  `client_name` derived from it (`src/core/runtime.c:609`). The runtime already
  validates its *shape* (`src/core/runtime.c:494`) and nothing more; it is
  client-asserted and defaults to absent.
- Any business header the client controls. The 2026-07-28 revision mirrors
  `method`, `params.name` and annotated tool parameters into `Mcp-Method`,
  `Mcp-Name` and `Mcp-Param-*` headers. Those are **routing** metadata written
  by the client, and the spec's own header/body validation exists precisely
  because two components trusting different copies of a client-controlled value
  is a vulnerability. They are never identity.
- A reverse proxy's forwarded-identity header, **unless** the transport
  independently verified the proxy — mutual TLS to the proxy, or a unix socket
  only the proxy can open. An unverified `X-Forwarded-User` is a request
  parameter with a respectable name.

`docs/middleware.md` already states the consequence for policy — `on_authorize`
"decides on the resolved identity and on the channel's embedder-bound
principal, never on the client-asserted `clientInfo.name`"
(`docs/security-model.md:20`). This document supplies the other half: what
actually puts something trustworthy in that slot.

## Authentication is per transport unit, not per connection

On a transport where one connection carries one conversation, "per connection"
and "per principal" coincide, and stdio has never had to tell them apart.
Modern HTTP breaks the coincidence: **each POST is independent, so each POST is
authenticated on its own**, and two POSTs on one kept-alive TCP connection may
legitimately carry two different principals or one valid and one revoked.

The rule is therefore stated against the *transport unit* — one POST, one
stdio process, one accepted unix-socket connection — and the binding is:

```
one transport unit  →  one authenticate() call  →  one principal
                    →  one channel bound to it  →  one channel_destroy
```

This is not an efficiency compromise. Authenticating once per TCP connection
and reusing the result would resurrect a session by the back door, on a
transport whose whole design premise is that there is no session — and it would
make token revocation take effect only when a client happens to reconnect.

The cost is real and must be measured rather than assumed: an authenticator
doing a network call per POST would dominate the request. That is the
authenticator's problem to solve (a cache keyed on the credential, behind its
own lock, with its own TTL), and it is exactly why `authenticate` is a hook and
not runtime code. The runtime's contract is "called every time"; how expensive
that is belongs to the implementation behind the pointer.

## Authentication runs before the body

**Decided: the credential is checked as soon as the headers are complete, and
before a single byte of the request body is read.** The order is:

```
headers complete and within limits
  → Origin / Host validation
  → build credentials from headers
  → authenticate()
  → (only now) read the body
  → parse JSON
```

The alternative — read the body, then authenticate — lets an unauthenticated
caller hold a server thread for the whole body-read deadline and push up to the
body limit before being told no. That is a denial-of-service primitive handed
out for free, and it is the reason this ordering is a contract rather than an
implementation detail: it is why `maelys_mcp_transport_credentials_t` carries no
view of the body and cannot be built from one.

One consequence has to be handled honestly rather than waved through, because
the naive reading of "respond without consuming the body" produces a bug.
Replying `401` and closing immediately while the client is still writing its
body sends a TCP RST, and on most stacks the client then loses the response it
was supposed to read — so the caller sees a connection error instead of an
authentication failure, and retries. The transport therefore does this:

1. write the response;
2. `shutdown(fd, SHUT_WR)`;
3. read-and-discard at most a small fixed amount (8 KiB) with a short deadline —
   **not** the body limit, and **not** the body deadline;
4. close.

That preserves the property the ordering exists for (no thread is held for the
body deadline, no body-sized buffer is ever allocated for an unauthenticated
caller) while still delivering the answer. `Expect: 100-continue` is the
standard mechanism for exactly this problem and is answered `417` in v1 — see
the transport document's parser rules, and the note there on why supporting it
is the natural later refinement rather than a gap.

## Outcomes, and the status each one gets

Decided, because the three are routinely conflated and conflating them is how
an outage becomes an authorization bypass.

| Outcome | Meaning | HTTP | Notes |
|---|---|---|---|
| credential absent | no `Authorization` at all | `401` | `WWW-Authenticate: Bearer` |
| credential invalid | present, and rejected | `401` | `WWW-Authenticate: Bearer` |
| authenticator cannot decide | backend down, timeout, internal fault | `503` | `Retry-After` when known |
| principal denied by `on_authorize` | authenticated, then refused | `403`, or the MCP policy error | see below |

**An invalid bearer is a failed authentication, not a denied principal.** It
gets `401`, never `403`. The distinction is not pedantry: `403` means "we know
who you are and you may not do this", which tells a caller to stop retrying with
this credential and escalate; `401` means "we do not know who you are", which
tells it to obtain a credential. Answering `403` to a bad token sends the caller
to the wrong remedy, and answering `401` to a real denial invites a credential
refresh loop against a decision that will not change.

`WWW-Authenticate: Bearer` is the minimum, and it is what the MCP authorization
specification expects a protected resource to emit. It is the extension point
for the OAuth work that is out of scope here: the challenge later grows a
`resource_metadata` parameter pointing at the protected-resource metadata
document, which is an addition to this header and not a change to this contract.
v1 emits the bare challenge and adds no realm.

**"Could not decide" is `503`, never `401` and never `403`.** An authenticator
whose backend is unreachable has not established that the caller is anonymous
and has not established that it is forbidden. Mapping that to `401` would
silently downgrade every request to unauthenticated during an outage; mapping it
to `403` would be a lie about a decision nobody made. This mirrors the rule the
middleware chain already took for the same reason: a hook that cannot reach a
verdict fails the request rather than being read as a denial
(`docs/security-model.md:21`).

The last row is where the two layers meet. A request that authenticated
successfully and is then refused by `on_authorize` is **not** an HTTP-layer
failure — it is a normal, well-formed MCP exchange whose answer is a refusal, so
it keeps HTTP `200` and carries the runtime's existing `-32003`
(`src/internal/internal.h:30`) in the body, exactly as it does over stdio. HTTP
`403` is reserved for a refusal that happens *before* dispatch — an authenticated
principal that this endpoint will not serve at all. Keeping the two apart is
what stops a transport from having an opinion about per-operation policy, and it
means a middleware's denial behaves identically on both transports.

## Channel-context ownership, and ABI 4

**Decided by the repository owner.** `maelys_mcp_channel_config_t` gains the
ownership fields directly, and `MAELYS_MCP_ABI_VERSION` goes from `3`
(`include/maelys/mcp/version.h:19`) to `4`.

```c
typedef struct maelys_mcp_channel_config {
    size_t max_messages;
    size_t max_bytes;
    size_t response_burst;
    unsigned int admission_timeout_ms;
    unsigned int close_timeout_ms;

    /* Opaque, embedder-owned; unchanged. */
    void *context;

    /*
     * Which protocol eras this channel serves and announces. Zero means
     * MAELYS_MCP_ERA_ALL, so a memset-and-fill embedder gets exactly today's
     * behaviour. Fixed at creation and never mutated afterwards.
     */
    unsigned int protocol_eras;

    /*
     * Called exactly once, when `context` stops being reachable by anything -
     * on the synchronous destroy path and on the detached one alike, on
     * whichever thread performs the real free. NULL means the runtime owns
     * nothing and calls nothing, which is today's behaviour.
     */
    void (*context_release)(void *release_context, void *context);
    void *release_context;
} maelys_mcp_channel_config_t;
```

`maelys_mcp_channel_set_protocol_eras` — which shipped in 0.17.0
(`include/maelys/mcp/channel.h:94`) — is **removed**, not deprecated.

### Why a field and not a setter

The shipped header poses this exact question and defers it to the owner, which
is worth quoting because it means nothing here is being relitigated:

> This is a setter rather than a `maelys_mcp_channel_config_t` field or a new
> constructor on purpose. Widening that released public structure would be an
> ABI break (`docs/abi-policy.md`), and which permanent public shape the
> capability should eventually take — a field behind an ABI 4 bump, or a
> size-prefixed options struct behind a `_ex` constructor — is an open question
> for the repository owner.
>
> — `include/maelys/mcp/channel.h:84`

The answer is the first branch, and the rationale is specific rather than
aesthetic: **there are no external users yet**, so the migration cost of an ABI
break is nil, and at zero cost the cleanest API wins. The owner's stated
boundary is equally specific and is recorded so a later reader does not mistake
this for a general appetite: he would otherwise have reserved an ABI bump for a
*bundle* of structural changes on the 1.0 runway — typed identity, cancellation
policy, transport options. **Those remain future work and are deliberately not
in this bump.** ABI 4 is these fields and this removal, nothing else.

Removing the setter is a benefit of the change, not a casualty of it. A setter
on a live channel needs state rules, and the shipped one has them: it refuses
on a closing or faulted channel, and refuses to withdraw
`MAELYS_MCP_ERA_LEGACY` from a channel that has already accepted an
`initialize`, "because a negotiated era cannot be taken back from a client that
is already using it" (`include/maelys/mcp/channel.h:79`). It also forces the
field to be mutable under lock — `src/internal/internal.h:284` records that it
is "written and read under `channel->mutex`, because a dispatch on a worker
thread reads it while the transport that created the channel may still be
setting it up". A config field is written once before the channel is published
and read-only thereafter, so **every one of those rules simply ceases to
exist**. That is a smaller runtime, not merely a different spelling.

There is no `maelys_mcp_channel_create_ex`, no options struct and no `size`
field. That alternative is withdrawn, which also keeps this surface consistent
with `docs/launch-contract-design.md:253`'s refusal of `struct_size` on the
launcher: the project's one answer to extending a released structure is the ABI
constant, checkable through `maelys_mcp_abi_version()`, rather than
self-describing structs that silently tolerate mismatched builds.

### Where `context_release` runs

Exactly one place, and the shipped code already says which:

> It is also the single point at which a channel's context stops being
> reachable, and therefore the one place a context lifecycle callback belongs
> when the channel grows one. An authenticated transport principal is the case
> that needs it: the reaper carries a reference across the detach and releases
> it at the real free, which is here and is emphatically not the return from
> destroy.
>
> — `src/core/channel.c:218`, on `free_channel_storage`

`free_channel_storage` (`src/core/channel.c:227`) has exactly two callers —
`free_detached_channel` (`src/core/channel.c:262`) and the synchronous
`destroy_channel` (`src/core/channel.c:1034`) — so "exactly once, whichever
path destruction took" is one line added in one function, which is precisely
why that function was split out. No call site outside it may invoke the
callback, and a test must assert the count is 1 on both paths.

Three consequences to state rather than leave inferable:

- **The callback may run on a thread the embedder never created.** For a
  detached channel it runs on whichever in-flight operation finishes last
  (`include/maelys/mcp/channel.h:150`) — a provider's thread, in practice. It
  must therefore be safe to call from anywhere and must not assume it can
  reach thread-local state belonging to the request that minted the principal.
- **It may run after `maelys_mcp_channel_destroy_detached` has returned**, and
  the embedder gets no notification of when. That is the whole point: the
  connection is already gone. The release is the *only* signal, so an
  authenticator must not also try to free the principal on the return from
  destroy.
- **It runs before `maelys_mcp_runtime_destroy` returns**, because that call
  drains `detached_channel_count` (`src/core/runtime.c:161`) and the retirement
  happens after the free. So "every principal has been released by the time the
  runtime is gone" remains true, and is now true *promptly* rather than only
  eventually.

## Lifetime, and the one ordering that matters

```
authenticate()                     → principal, refcount 1, held by the transport
retain()                           → refcount 2, the channel's own reference
channel_create(config = {
    .context         = principal,
    .context_release = release_trampoline,
    .release_context = authenticator,
    .protocol_eras   = MAELYS_MCP_ERA_MODERN })
    ... dispatch; hooks read maelys_mcp_channel_context() ...
channel_destroy_detached()
    ├─ closed cleanly  → freed inline; context_release ran before this returned
    └─ ERR_TIMEOUT     → detached; context_release will run at the real free,
                         on some other thread, at some later time
release()                          → the transport's own reference
```

The channel's reference is now genuinely the channel's: it is taken before
creation and given back by `context_release`, wherever and whenever that
happens. The embedder no longer has to reason about which destroy path ran,
which is what made the previous draft's version wrong.

Three precisions that are easy to get wrong:

- **`release` may run on a thread that is not the one that authenticated**, and
  under detached destruction it usually will not be. Both refcount operations
  are declared thread-safe for exactly this reason.
- **A principal can outlive its request, and the bound is the provider's.**
  The free happens once the last in-flight operation returns, which for the
  process provider is bounded by `call_timeout_ms`
  (`src/internal/internal.h:444`) and for an in-process provider is bounded by
  nothing — a gap `docs/launch-contract-design.md:1135` confirms is deliberate,
  in-process providers being inside the trust boundary by definition. Detaching
  is what stops that from holding a connection hostage; it does not, and
  cannot, shorten the provider itself.
- **`context_release` must tolerate being the last thing that touches the
  principal.** It is not a notification that something else will clean up; it
  is the cleanup.

## v1 authenticators, and where the later ones plug in

Two ship, and neither is interesting on purpose — the value of this cycle is
the seam, not the credentials.

| Authenticator | `kind` | Principal | Failure modes |
|---|---|---|---|
| `loopback-trust` | `NONE` | one process-wide singleton, refcounted but never freed | none; refuses to be installed on a non-loopback bind |
| `static-bearer` | `BEARER` | one per configured token, allocated at configuration time | absent header → `ERR_ARGUMENT`; non-matching → `ERR_DENIED` |

`static-bearer` compares in constant time and never logs the token or any
prefix of it. It is a *test and single-tenant* mechanism, and the document that
introduces it should say so; a runtime whose only network credential is a
shared secret in a config file is not a multi-tenant server.

The later ones need no new seam, which is the property this design is being
judged on:

- **mTLS** — the listener terminates TLS, verifies the chain against a
  configured CA, and passes `kind = TLS_CLIENT` with the DER-encoded peer
  certificate as `material`. The authenticator maps subject to principal.
- **Unix-socket peer** — the listener calls `getsockopt(SO_PEERCRED)` (Linux)
  or `getpeereid()` (BSD/macOS) and passes `kind = PEER` with a
  `struct { uid_t uid; gid_t gid; pid_t pid; }` as `material`. No parsing, no
  ambiguity, no forgery.
- **OAuth resource server** — a bearer authenticator that validates a JWT
  offline or introspects it, caching behind its own lock. It is
  `static-bearer` with a different comparison, which is the argument that the
  `kind = BEARER` shape is sufficient.

None of the three requires a `headers` view, which is the check on whether the
credentials type was drawn narrowly enough.

## Per-principal effect policy

The roadmap couples "authenticated principals" and "per-principal effect
policy" in one sentence, and the coupling is right, but the second half needs
**no runtime change at all** — which is worth stating loudly, because it is the
cheapest good news in this design.

Effect gating today lives in the reference host: `apply`, `commit` and
`execute` are denied unless enabled with `--allow-effect`
(`docs/security-model.md:13`), runtime-wide. Once the channel context carries a
principal, an `on_authorize` middleware reads it through
`maelys_mcp_channel_context()` and consults the principal's own effect set. The
hook already receives the channel and already runs before schema validation
(`docs/security-model.md:23`), and a test in the middleware suite already
demonstrates the shape — denying on one channel and allowing on another within
one runtime (`docs/middleware-design.md:266`).

So: per-principal effect policy is a *host* feature built on this contract, not
a second runtime mechanism. The library keeps mechanism only. That is the same
conclusion `docs/middleware-design.md:560` reached about declarative
configuration, and it should not be re-litigated per feature.

## What `docs/security-model.md` gains

Five bullets under **Defaults**, to be added when this ships:

- The transport principal is established at channel creation from a credential
  the transport itself authenticated — a validated bearer token, a verified TLS
  client certificate, kernel-supplied socket peer credentials, or an explicitly
  configured local identity. It is bound to the channel's opaque context for
  the channel's lifetime and is never derived from payload metadata:
  `clientInfo.name`, `_meta`, and the client-written `Mcp-Method` / `Mcp-Name` /
  `Mcp-Param-*` routing headers are advisory and carry no authority. A
  forwarded-identity header from a reverse proxy counts as payload unless the
  transport independently verified the proxy.
- Authentication is repeated for every independent transport unit. On a
  stateless HTTP transport that means every POST, so a revoked credential stops
  working on the next request rather than on the next connection.
- The credential is checked as soon as the request headers are complete and
  before any request body is read, so an unauthenticated caller can neither hold
  a server thread for the body deadline nor cause a body-sized allocation. A
  rejected request is answered and closed after discarding at most 8 KiB of
  unread body under a short deadline, which is what makes the answer reach the
  caller instead of being lost to a reset.
- An absent or invalid credential is a failed authentication and is answered
  `401` with a `WWW-Authenticate: Bearer` challenge — never `403`. An
  authenticator that cannot reach a verdict fails the request with `503`; it is
  never read as anonymous and never read as a denial. `403` is reserved for an
  authenticated principal this endpoint refuses to serve at all; a
  per-operation policy refusal stays an MCP-level `-32003` in a `200` response,
  identically on every transport.
- Multi-tenant MRTR continuations are not supported. `requestState` must be
  bound to the operation, arguments, expected round, expiration **and
  principal** before a provider acts on it; until that binding exists, only a
  `loopback-trust` or genuinely mono-tenant single-token configuration may be
  described as safe, and the host refuses a multi-token configuration with an
  MRTR-capable provider unless the operator explicitly acknowledges the
  restriction.

Two replacements as well: the paragraph at `docs/security-model.md:82`, whose
"does not expose HTTP" clause stops being true in the same release and whose
closing clause about the per-channel context being "the foundation a
transport-established principal will bind to" (`docs/security-model.md:85`)
stops being future tense; and the `requestState` bullet at
`docs/security-model.md:49`, whose binding list is missing the principal term.

## The migration entry `docs/abi-policy.md` requires

`docs/abi-policy.md:19` permits a pre-1.0 ABI break only if it "must increment
`MAELYS_MCP_ABI_VERSION` and document the migration in `CHANGELOG.md`". Drafted
here so it ships with the change rather than after it, in the shape the ABI 2 →
3 entry established (`CHANGELOG.md:295`):

> - **API break (ABI 3 → 4), migration is two struct fields.**
>   `maelys_mcp_channel_config_t` gains `protocol_eras`, `context_release` and
>   `release_context`, and `maelys_mcp_channel_set_protocol_eras` — added in
>   0.17.0 — is removed. To migrate, set `.protocol_eras` in the config you
>   already pass to `maelys_mcp_channel_create` instead of calling the setter
>   after it; zero keeps `MAELYS_MCP_ERA_ALL`, so a caller that never restricted
>   eras changes nothing but a recompile. The setter's state rules disappear
>   with it: there is no longer a window in which a channel's era set can be
>   narrowed after traffic has started, and therefore no "cannot withdraw
>   `MAELYS_MCP_ERA_LEGACY` after `initialize`" case to handle.
> - **New: the channel context can own something.** `context_release`, when
>   set, is called exactly once with `release_context` and `context` at the
>   moment the context stops being reachable — on the synchronous destroy path
>   and on `maelys_mcp_channel_destroy_detached`'s deferred one alike, on
>   whichever thread performs the real free. NULL preserves today's behaviour,
>   in which the runtime owns nothing. This closes the gap 0.17.0's detached
>   destruction left open, where an embedder had to wait for
>   `maelys_mcp_runtime_destroy` to reclaim a detached channel's context.

The break is taken now for one stated reason — there are no external users, so
the migration costs a recompile — and its scope is deliberately these fields
and this removal. The structural changes the owner would otherwise have
bundled into an ABI bump (typed identity, cancellation policy, transport
options) are **not** in it and remain on the 1.0 runway.

`docs/abi-policy.md` also needs its own correction in the same change, and it
is not this design's doing: `docs/launch-contract-design.md:278` already
recorded that the file "documents ABI 1 → 2 at `:32-36` and never mentions ABI
3, which shipped". Adding an ABI 4 paragraph on top of a file that never
documented ABI 3 would compound the gap, so the ABI 3 paragraph lands with it.

## Restriction: multi-tenant MRTR continuations are not supported

This is a **restriction on supported configurations**, not a backlog item, and
it is stated here rather than in a "not in v1" list because a reader who treats
it as a future nicety will deploy something unsafe.

`requestState` and `inputResponses` are opaque provider blobs round-tripped
through the client, and a continuation may legitimately arrive on a different
channel than the one that issued it (`docs/middleware-design.md:407`). Over
stateless HTTP that is not an edge case: **every** continuation arrives on a
different channel, because the second POST of an MRTR exchange is a new
connection, a new channel and a fresh `authenticate()` call. Channel-scoped
identity therefore fences nothing here. Principal A can be handed — or can
steal, or can be socially engineered into replaying — a `requestState` minted
for principal B, and present it on its own authenticated channel.

`docs/security-model.md:49` already requires a mutating provider to authenticate
`requestState` and bind it to "the operation, arguments, expected round and
expiration". That list is missing one term, and the missing term is the one this
document is about: **the principal**. Until `requestState` is cryptographically
bound to the principal that minted it — an HMAC or AEAD over the principal
identifier under a server-held key, verified before the continuation is acted
on — a continuation proves what it is for and not who it belongs to.

The supported configurations follow directly, and there are exactly two:

- **`loopback-trust`.** One principal by construction, so cross-principal
  continuation is not expressible. Safe.
- **A genuinely mono-tenant `static-bearer`** — one token, representing one
  principal. Safe, for the same reason.

If more than one token is configured, or one token is shared by parties that are
not the same principal, then MRTR continuations are **not** guaranteed, and a
deployment has two honest choices: refuse cross-principal continuation outright
(reject a `requestState` whose channel's principal differs from the one recorded
at issue, which requires the provider to record it), or accept the risk
knowingly. What is not available is describing that configuration as safe. The
CLI must refuse to start a multi-token configuration together with any
MRTR-capable provider unless the operator passes an explicit
acknowledgement flag, so the restriction is enforced at configuration time
rather than documented and forgotten.

Closing this properly is a continuation-identity cycle of its own. It is named
here so that the cycle has a definition and the restriction has an owner.

## Not in v1, named rather than hidden

- **Authorization-server integration.** No OAuth metadata endpoint, no token
  introspection, and no `resource_metadata` parameter on the challenge. The
  bare `WWW-Authenticate: Bearer` challenge ships; the parameter that points at
  a protected-resource metadata document is an addition to that header, which
  is why emitting the challenge now is the extension point rather than a
  placeholder.
- **Principal-scoped catalogs are possible but unbuilt.** `on_list` filtering
  per principal is expressible the day the slot is filled
  (`docs/middleware-design.md:157`). No middleware in this repository does it.
- **No principal in the audit record's own fields.** `on_audit` reaches the
  principal through the channel like everyone else. Adding a typed field to the
  audit context would widen a released structure for no capability.
