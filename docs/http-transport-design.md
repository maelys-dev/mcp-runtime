# HTTP transport — design

> **Status: implemented.** Every phase this document plans has shipped, and the
> binary serves MCP over HTTP directly. What remains here is the record of the
> decisions and their reasons, kept so the implementation can be reviewed
> against them rather than against memory — the form `docs/middleware-design.md`
> established.
>
> Sections that a shipped piece has overtaken carry an **As built** note saying
> what actually happened, including where reality and the design diverged. The
> design text is left standing rather than rewritten into a description: a
> document that quietly edits its predictions to match the outcome is no longer
> evidence of anything.
>
> Verified against `022a4c4` (0.18.0 plus H1–H4). Every code claim carries a
> `file:line` citation re-read against that commit.
> `docs/middleware-design.md` is the precedent for this form, and it is also
> the cautionary tale: three of its draft invariants were false until someone
> read the source. Fourteen of this document's draft claims were wrong the same
> way — including one that made its whole cancellation chain unrealizable and
> one that made its adapter API unimplementable from outside the library — and
> every correction is recorded in
> [Where the code contradicted the drafts](#where-the-code-contradicted-the-drafts)
> rather than quietly folded in.

## Contents

1. [What shipped, and where](#what-shipped-and-where)
2. [Layering: server, adapter, runtime](#layering-server-adapter-runtime)
3. [The era rule, and the per-channel version mask](#the-era-rule-and-the-per-channel-version-mask)
4. [The wire contract](#the-wire-contract)
5. [Runtime prerequisites](#runtime-prerequisites)
6. [Internal architecture](#internal-architecture)
7. [The parser, and its fuzz targets](#the-parser-and-its-fuzz-targets)
8. [Security model delta](#security-model-delta)
9. [Phasing, and what each phase authorizes](#phasing-and-what-each-phase-authorizes)
10. [Not in v1](#not-in-v1)
11. [Open questions](#open-questions)
12. [Where the code contradicted the drafts](#where-the-code-contradicted-the-drafts)
13. [Readiness](#readiness)

## What shipped, and where

This document was first written against 0.16.0, when none of it existed. It is
now the design of record for a transport that is in the tree. The ledger comes
first because reading the phase plan below as future work would be wrong:

| Piece | Where |
|---|---|
| Detachable channel destruction | 0.17.0 (#55) — `maelys_mcp_channel_destroy_detached` (`include/maelys/mcp/channel.h:188`) |
| Generic process launcher | 0.17.0 (#58), `docs/launch-contract-design.md` |
| Host-enforced fd-3 stdout isolation | 0.17.0 (#59) |
| Authenticated-principal contract | 0.18.0 (#63), `docs/authenticated-principal-design.md` |
| ABI 4 channel config, era mask as a field, **pollable outbox** (H0a) | 0.18.0 (#64) |
| **H1** — server layer, adapter seam, request parser | #67 |
| **H2** — MCP routing-header validation and the Base64 sentinel | #69 |
| **H3** — dispatch, JSON/SSE mode selection, cancellation, shutdown | #70 |
| **H4** — the official runner drives the real HTTP endpoint directly | #72 |

Two structural notes about how that differs from the plan.

**The phase boundary moved once, at the orchestrator's direction.** The design
put the authenticator seam in H2 and the parser in H1; as built, **H1 absorbed
the authenticators** — `host/http_auth.c` landed with the server layer — so H1
delivered listener, parser, adapter seam *and* `loopback-trust` /
`static-bearer`, and H2 was the MCP-facing half alone: `MCP-Protocol-Version`,
`Mcp-Method`, `Mcp-Name`, the Base64 sentinel and the header/body comparison.
The [ladder](#the-authorization-ladder) below is the pre-build reading and is
left as written; this is the as-built boundary.

**Principal binding happened in H3, not H2.** The ladder says H2 "binds
principals to channels", which was never quite right: H2 created no channel.
Authentication ran in H1, validation in H2, and the first `channel_create`
carrying a principal is in H3 (`src/transport/http_adapter.c:1127`) — because
that is the phase in which channels first exist at all.

## Layering: server, adapter, runtime

Three layers, two of which are new, with a real API between them. The split is
not tidiness: an orchestrator must be able to reuse the MCP half behind its own
listener, its own TLS terminator, or a reverse proxy, without inheriting a
socket implementation it did not ask for — and the standalone binary must still
work with no external server at all.

```
┌─ host/http_server.c ────────────────────── the HTTP server layer ─┐
│  listen, accept, connection lifetime                              │
│  bounded request-line and header parsing                          │
│  path routing  ·  Origin/Host validation  ·  limits               │
│  authenticate() → principal                                       │
│  reads the body only after that succeeds                          │
└───────────────────────────┬───────────────────────────────────────┘
                            │  maelys_mcp_http_request_t (parsed, authenticated)
                            │  maelys_mcp_http_response_writer_t (embedder's)
┌───────────────────────────▼─── src/transport/http_adapter.c ──────┐
│  the HTTP↔MCP adapter, in the library                             │
│  header↔body validation  ·  request/notification classification   │
│  channel_create(principal) · channel_accept · outbox drain        │
│  JSON-or-SSE mode selection · cancellation · shutdown ordering    │
└───────────────────────────┬───────────────────────────────────────┘
                            │  existing public channel API
┌───────────────────────────▼───────────────────────────────────────┐
│  the runtime                                                      │
└───────────────────────────────────────────────────────────────────┘
```

The adapter's inbound contract is that it never touches a socket, and the API
is shaped so that it *cannot* — there is no descriptor in it, and nothing it
receives would let it distinguish a FIN from a pipelined byte.

```c
/* One in-flight exchange. Opaque; created by the adapter, valid from the
 * moment _handle is entered until it returns. Carries the request and the
 * channel, so a canceller never has to name either. */
typedef struct maelys_mcp_http_exchange maelys_mcp_http_exchange_t;

/* Read-only access to the parsed header block. The embedder supplies the
 * lookup, because it already parsed the headers and the adapter must not
 * assume a representation. Returns 1 and fills *out_value/*out_length when
 * present, 0 when absent. `name` is ASCII and matched case-insensitively.
 * A header the embedder saw more than once must be reported as absent - the
 * server layer refuses duplicates before the adapter is reached, and a lookup
 * that silently picked one would undo that. */
typedef int (*maelys_mcp_http_header_lookup_fn)(
    void *context,
    const char *name,
    const char **out_value,
    size_t *out_length);

typedef struct maelys_mcp_http_request {
    const char *method;              /* "POST"; the server layer rejects others */
    const char *path;                /* already routed; informational */
    /* The adapter reads MCP-Protocol-Version, Mcp-Method and Mcp-Name through
     * this and nothing else. It never asks for Authorization. */
    maelys_mcp_http_header_lookup_fn header_lookup;
    void *header_context;
    const void *body;
    size_t body_length;
    /* Established by the server layer. Borrowed for the call; the server layer
     * owns the retain/release pair (docs/authenticated-principal-design.md). */
    maelys_mcp_principal_t *principal;
    /*
     * An ABSTRACT cancellation source: a descriptor that becomes readable when
     * this exchange has been cancelled, and stays readable. What made it
     * readable is the embedder's business - a client FIN, an admin abort, a
     * deadline. The adapter adds it to its poll set and reads nothing from it.
     *
     * -1 means the embedder cancels out-of-band instead, by calling
     * maelys_mcp_http_exchange_cancel from another thread. An embedder that
     * offers neither gets no cancellation, and _handle says so in its result
     * rather than pretending otherwise.
     */
    int cancel_fd;
} maelys_mcp_http_request_t;

typedef enum maelys_mcp_http_stream_end {
    /* The exchange finished normally: write the terminal chunk. */
    MAELYS_MCP_HTTP_STREAM_COMPLETE = 0,
    /* The exchange was abandoned: do NOT write the terminal chunk; close the
     * connection so the peer sees a truncated body. */
    MAELYS_MCP_HTTP_STREAM_ABORTED = 1
} maelys_mcp_http_stream_end_t;

typedef struct maelys_mcp_http_response_writer {
    void *context;
    /* Exactly one of begin_json / begin_stream is called, at most once. */
    maelys_mcp_result_t (*begin_json)(void *ctx, int status,
        const char *body, size_t length);
    maelys_mcp_result_t (*begin_stream)(void *ctx);
    /* One SSE event. Must flush before returning. */
    maelys_mcp_result_t (*write_event)(void *ctx, const char *json, size_t length);
    /* An SSE comment keep-alive. NULL is legal and means "no keep-alives". */
    maelys_mcp_result_t (*write_keepalive)(void *ctx);
    /* Ends a stream begun by begin_stream. Called exactly once, with the
     * disposition. See "Chunked framing of SSE responses" for the exact bytes
     * each disposition writes. */
    maelys_mcp_result_t (*end_stream)(void *ctx,
        maelys_mcp_http_stream_end_t disposition);
    /* A status-only reply with no MCP body: 405, 415, 406, 411, 417, 503. */
    maelys_mcp_result_t (*status_only)(void *ctx, int status,
        const char *const *extra_headers, size_t extra_header_count);
} maelys_mcp_http_response_writer_t;

maelys_mcp_result_t maelys_mcp_http_adapter_handle(
    maelys_mcp_http_adapter_t *adapter,
    const maelys_mcp_http_request_t *request,
    const maelys_mcp_http_response_writer_t *writer,
    /* Published before any dispatch, so an out-of-band canceller has something
     * to name. NULL when the caller does not intend to cancel. */
    maelys_mcp_http_exchange_t **out_exchange);

/* Callable from any thread while _handle is running. Idempotent. After
 * _handle returns, the handle is dead and calling this is a no-op rather than
 * a use-after-free: the adapter retires it before returning. */
void maelys_mcp_http_exchange_cancel(maelys_mcp_http_exchange_t *exchange);
```

> **As built, the sketch above is the shipped header** — `end_stream`'s
> disposition, the opaque exchange handle, the `header_lookup` callback and the
> abstract `cancel_fd` all landed with these signatures
> (`include/maelys/mcp/http.h:135`, `:232`, `:62`, `:117`). Three things were
> added that the design did not list, and each closes a real gap:
>
> **A graceful-shutdown seam beside the cancellation one.** `shutdown_fd`
> (`include/maelys/mcp/http.h:132`) and
> `maelys_mcp_http_exchange_shutdown` (`:248`) are the same shape as the
> cancellation pair and mean the opposite thing: cancellation asks an exchange
> to *stop*, shutdown asks it to *end*. A shut-down exchange completes every
> surviving subscription with `resultType: "complete"`, writes those frames,
> and terminates the stream properly — because the peer is still connected and
> is owed an ending — whereas a cancelled one writes nothing further and leaves
> the body truncated on purpose. The design described server shutdown as a
> phase ordering but never gave the adapter an entry point for it, so an
> embedder had no way to say "finish, don't abort". One descriptor may be
> shared by every in-flight exchange, which is what a server-wide stop is.
> A cancellation already recorded wins over a shutdown, because a peer that is
> gone cannot be given a graceful ending (`:245`).
>
> **A wakeup pipe for embedders with no descriptors.** An exchange that
> supplied neither `cancel_fd` nor `shutdown_fd` is woken by
> `maelys_mcp_http_exchange_cancel` itself, through a pipe the adapter
> allocates for exactly that case (`include/maelys/mcp/http.h:224`). Without
> it, an out-of-band canceller's request would have been noticed only at the
> next keep-alive tick — cancellation latency bounded by a keep-alive interval,
> which is the defect [H0a](#h0a--a-pollable-outbox) rejected mechanism (c)
> for having.
>
> **An explicit principal refcount pair.** `principal_retain` /
> `principal_release` / `principal_context`
> (`include/maelys/mcp/http.h:103`) are how the adapter takes its *own*
> reference rather than borrowing the server layer's: the server layer releases
> when `_handle` returns, the channel releases when it is really freed, and for
> a detached channel those are different instants — so each needs its own
> reference. The design said the principal was "borrowed for the call" and left
> the second reference implicit.

Three shapes here are deliberate and each replaces something that was wrong in
an earlier revision.

**The cancellation source is abstract.** An earlier revision handed the adapter
a `disconnect_fd` and had it call `recv(..., MSG_PEEK)` to tell a FIN from
pipelined data — while also claiming the adapter never touches a socket. Both
could not be true. Distinguishing a FIN from a pipelined byte is HTTP-server
knowledge and it stays in the server layer, which owns the
[pipelining rule](#keep-alive-and-pipelining) that makes the distinction
matter. The adapter learns one fact — *this exchange is cancelled* — and learns
it the same way whether the cause was a disconnect, an operator, or a timeout.
An embedder behind a framework with no socket at all can therefore implement
cancellation with `maelys_mcp_http_exchange_cancel` and never own a descriptor.

**Cancellation names an exchange, not a request.** `_cancel(adapter, request)`
made the request struct's *address* a concurrent identity, which is a race the
moment `_handle` returns and the caller reuses or frees that struct — the
canceller would be naming a dead pointer, or worse, a live one belonging to the
next request. An opaque handle carries the request's and the channel's lifetime
and is retired by the adapter before `_handle` returns, so a late cancel is a
no-op by construction rather than by the caller's discipline.

**Header access is a callback, not an opaque struct.** `maelys_mcp_http_headers_t`
as an opaque type with no public accessor was unimplementable from outside the
library: an orchestrator could neither build one nor read one. A lookup
function is the smallest thing that lets an embedder expose whatever
representation it already has — a slice list, a hash map, a framework's own
header object — without copying it, and it keeps the adapter from depending on
a layout. The duplicate rule is part of the contract rather than an
implementation note, because the header/body validation the adapter performs is
only sound if "present once" is what a hit means.

Three properties this buys, each of which is why the split is worth an extra
header:

- **The standalone binary owns routing and the path.** `/mcp` is the default
  of the *host*, not of the library, and a host that later wants `/healthz`, a
  second runtime on `/mcp/v2`, or a prefix behind a proxy changes a CLI flag
  rather than a transport. The adapter is handed an already-routed request and
  has no opinion about paths.
- **An orchestrator reuses the adapter verbatim.** It implements
  `maelys_mcp_http_response_writer_t` over whatever it already has — a Go
  `http.ResponseWriter` through cgo, a libevent connection, an nginx module —
  and gets JSON/SSE selection, ordering, cancellation and shutdown semantics
  identical to the standalone binary's, because it is the same code.
- **The fuzz targets get a seam that is not a socket.** `http-exchange` drives
  `maelys_mcp_http_adapter_handle` with a recording writer, which is why it can
  run a whole exchange per iteration without a listener.

The server layer lives in `host/`, not in the library. That follows the
conclusion `docs/middleware-design.md:560` already reached for configuration —
the library keeps mechanism, the host keeps policy — and it keeps `libmaelys_mcp`
free of `listen(2)`.

## The era rule, and the per-channel version mask

| Transport | Protocol versions served |
|---|---|
| stdio | `2024-11-05`, `2025-03-26`, `2025-06-18`, `2025-11-25`, `2026-07-28` |
| HTTP | `2026-07-28` **only** |

The runtime accepts every dated revision up to `2025-11-25` through the legacy
`initialize` handshake and `2026-07-28` through per-request `_meta`
(`docs/protocol-support.md:3`). This transport serves the second of those and
nothing else.

The argument is that the legacy era is not a *dialect* of this endpoint, it is
a different endpoint. Serving `2025-11-25` over HTTP would mean reintroducing,
all at once: the `initialize` / `notifications/initialized` lifecycle; a server
-assigned session and the `Mcp-Session-Id` header that carries it; HTTP DELETE
to terminate that session; a standalone GET SSE stream for server-initiated
messages; server-to-client JSON-RPC *requests* on that stream; and
`Last-Event-ID` resumption. Every one of those is a mechanism this design
deletes, and the deletions are what make it small. A transport that has to
support both is not this transport with an extra branch — it is two transports
in one file, and the second one is the one the 2026-07-28 revision removed on
purpose.

The rule is also enforced structurally rather than by a check, which is the
better kind of enforcement. Because a channel exists for exactly one POST (see
[Internal architecture](#internal-architecture)), `channel->legacy_initialized`
is `0` for every request that ever reaches dispatch, so any request without
modern `_meta` — other than `ping` and `initialize` themselves — falls into the
initialization gate and is answered `-32002 Server not initialized`. The
transport does not have to hunt for legacy traffic; legacy traffic cannot
survive the channel model. The design proposed to refuse the two methods that
*would* escape that at the HTTP boundary, with `400` and `-32601`.

> **As built, and better: the refusal lives where the guarantee lives.** Both
> methods reach dispatch and are refused by the **era-masked runtime**, not by
> the transport, and the observable answers are not the ones this section
> predicted:
>
> | Method | Design said | Shipped, and pinned by H3's tests |
> |---|---|---|
> | `initialize` | `400` + `-32601` at the boundary | **`200` + `-32600`** from the runtime |
> | `notifications/initialized` | `400` + `-32601` at the boundary | **`202`**, then dropped by the runtime's ordinary notification rules |
>
> The reasoning is in the source, and it is more careful than the design's was
> (`src/core/runtime.c:432`):
>
> > -32600 … on a channel that does not serve the legacy era the request is not
> > a bad handshake, it is a request that has no meaning here at all. -32602
> > would invite the client to retry with different params, -32022 would claim a
> > version was negotiated and rejected when none was, and -32601 is what this
> > runtime says about methods no module implements — which initialize is not.
> > **A transport in front of this may refuse the method earlier and more
> > cheaply; this is where the guarantee itself lives.**
>
> That last sentence is the correction. The design had the transport enforcing
> the era rule with the runtime as backup; the shipped arrangement is the
> reverse, and the reverse is right — the era mask is a channel property, so a
> channel that cannot be told to serve the legacy era cannot be talked into it
> by *any* request over *any* transport, and the HTTP boundary would be
> duplicating a guarantee it does not own. `src/transport/http_adapter.c:1105`
> makes the same point from the other side: setting
> `protocol_eras = MAELYS_MCP_ERA_MODERN` on the channel template "is where the
> era rule becomes structural rather than a promise".
>
> `-32600` also keeps its HTTP status at `200`, which is deliberate rather than
> incidental: the runtime saying "this request has no meaning on this channel"
> is an *answer*, not a transport failure, and the
> [status mapping](#status-and-error-mapping) reserves non-`200` for the
> transport's own refusals (`src/transport/http_adapter.c:738`).
>
> `notifications/initialized` gets `202` for the same structural reason it gets
> `202` on any other notification: it has no `id`, so dispatch returns nothing
> and there is no response to carry a refusal. The design's objection — that
> `202` for a meaningless message "is a lie a client can build on" — was
> overstated. `202 Accepted` is an acknowledgement of *receipt*, which is true,
> and the alternative would have meant the transport parsing method names to
> decide which notifications are permitted, which is exactly the
> method-name knowledge the layering keeps out of it.

`ping` deliberately answers in every state, before the initialization gate
(`src/core/runtime.c:694`), and that behaviour is kept: a liveness probe that
can be refused for lifecycle reasons is not a liveness probe. It still needs
its transport headers.

### The version mask, and what it fixes

**Shipped in 0.17.0, and its permanent shape is now decided.** The capability
landed in 0.17.0 (#55) as a setter, `maelys_mcp_channel_set_protocol_eras`,
and that header was explicit that the shape was provisional — quoted here from
`ca85409` because the setter and its comment are **gone from the tree**, which
is itself the outcome:

> This is a setter rather than a `maelys_mcp_channel_config_t` field or a new
> constructor on purpose. … which permanent public shape the capability should
> eventually take — a field behind an ABI 4 bump, or a size-prefixed options
> struct behind a `_ex` constructor — is an open question for the repository
> owner.
>
> — `include/maelys/mcp/channel.h:84`, as of `ca85409` (0.17.0)

The owner has answered: **a config field, behind ABI 4**, with the setter
removed rather than kept. The design and its rationale belong to PR #63
(`docs/authenticated-principal-design.md`), because the same ABI break carries
the context destructor and splitting one break across two releases would be
worse than either. What matters here is only the consequence:

```c
typedef struct maelys_mcp_channel_config {
    /* … existing fields, plus context_release/release_context from PR #63 … */
    unsigned int protocol_eras;   /* 0 ⇒ every era; HTTP passes ERA_MODERN */
} maelys_mcp_channel_config_t;
```

Three call sites read it, all of them already shipped and merely re-sourced
from the config instead of from the setter:

- `discover()` builds `supportedVersions` from the mask
  (`src/core/runtime.c:530`), so an HTTP channel advertises `["2026-07-28"]`
  and a stdio channel still advertises both.
- `initialize()` refuses with `-32600` when `MAELYS_MCP_ERA_LEGACY` is clear
  (`src/core/runtime.c:425`). The HTTP boundary's own refusal of `initialize`
  survives as a cheap early rejection, but it is no longer the only thing
  standing between an HTTP client and a legacy handshake — which is the right
  place for that guarantee to live.
- `validate_modern_metadata()` refuses with `-32022` when
  `MAELYS_MCP_ERA_MODERN` is clear (`src/core/runtime.c:562`).

Per-channel rather than per-transport, because the runtime has no notion of
which transport a channel came from and giving it one would breach a boundary
this codebase has kept clean (`scripts/audit_boundaries.sh`,
`src/internal/internal.h:440`). Per-channel rather than global, because one
runtime serves stdio and HTTP channels simultaneously and a global switch would
force a second runtime for a second transport.

Moving from setter to field was not cosmetic for this transport. The setter was
callable on a live channel, so the era set was mutable and 0.17.0's
`src/internal/internal.h` had to guard it under `channel->mutex` "because a
dispatch on a worker thread reads it while the transport that created the
channel may still be setting it up". A channel-per-request transport creates a
channel and dispatches into it immediately, which is exactly that window.

> **As built:** the field is written once before publication and read-only
> afterwards (`include/maelys/mcp/channel.h:81`), and that guard comment no
> longer exists in `src/internal/internal.h` because there is nothing left to
> guard. The window is gone rather than narrowed.

## The wire contract

### Endpoint and methods

One path. `POST` only. The path is the **host's** to choose and route
([layering](#layering-server-adapter-runtime)); `/mcp` is the standalone
binary's default, and the adapter never sees it as anything but informational.

| Method | Response |
|---|---|
| `POST` | see below |
| `GET`, `DELETE` | `405 Method Not Allowed`, `Allow: POST` |
| anything else | `405 Method Not Allowed`, `Allow: POST` |

`Mcp-Session-Id` is ignored and never minted or echoed. `Last-Event-ID` is
ignored; streams are not resumable. Both are ignored rather than rejected,
because rejecting them would break exactly the old client the ignoring is meant
to let fail cleanly at the version check instead.

### Inbound headers

| Header | Required | Rule |
|---|---|---|
| `Host` | yes | exactly one; must be present and syntactically valid; on a loopback bind must be a loopback authority. Absent, repeated, or empty → `400` |
| `Content-Type` | yes | must be `application/json`, optionally with a `charset=utf-8` parameter; anything else → `415` |
| `Accept` | yes | must list both `application/json` and `text/event-stream`; otherwise → `406` |
| `Content-Length` | yes | exactly one; decimal digits only; ≤ the body limit; otherwise → `400` |
| `Transfer-Encoding` | never | **any** value → `400`, connection closed. Present together with `Content-Length` → `400`, connection closed, logged as a smuggling attempt |
| `Expect` | no | any value → `417 Expectation Failed` |
| `MCP-Protocol-Version` | yes | must equal `2026-07-28` and must equal the body's `_meta` version |
| `Mcp-Method` | yes on requests | must equal the body's `method`, byte for byte |
| `Mcp-Name` | yes on `tools/call`, `resources/read` | must equal `params.name` / `params.uri` after sentinel decoding |
| `Mcp-Param-*` | never | not recognized by this server; forwarded-and-ignored per RFC 9110 (see below) |
| `Origin` | no | validated when present |
| `Authorization` | authenticator-dependent | never parsed by the transport itself; handed to `authenticate` as opaque material |

Header **names** are compared case-insensitively; header **values** are
compared case-sensitively — `tools/call` and `Tools/Call` are different
methods. A repeated occurrence of any header in the table is a `400`, not a
merge: `src/jsonrpc/core.c`'s own framing parser already takes that line
for `Content-Length` (`if (found || …) return -1`,
`src/jsonrpc/core.c:95`), and a transport that merged where the sibling parser
refuses would be the odd one out. Any header value containing a NUL, CR or LF
byte is a `400` before anything else looks at it — the same class of defect the
runtime already rejects wherever protocol strings enter length-unaware C APIs
(`docs/security-model.md:60`).

`Mcp-Name` is required only where the spec requires it. `prompts/get` is on the
spec's list and absent here, because there is no Prompts module
(`conformance/run_official_mcp.py:120`); a `prompts/get` POST is a `404` with
`-32601` and never reaches the name check.

### The Base64 sentinel

`Mcp-Name` may carry a value that is not a safe ASCII header value. The
encoding is exact and must be implemented exactly:

```
Mcp-Name: =?base64?{Base64EncodedValue}?=
```

The prefix `=?base64?` and the suffix `?=` are lowercase and case-sensitive.
Rules the parser implements, each of which is a rejection when violated:

- A value that starts with `=?base64?` **and** ends with `?=` is decoded;
  anything else is taken literally.
- The payload is standard Base64 with `=` padding. Non-alphabet characters,
  wrong padding length, and non-zero bits in the padding are all `400`. There
  is no URL-safe alphabet and no line wrapping.
- The decoded bytes must be valid UTF-8 and must contain no NUL. This is
  stricter than the spec, and it is deliberate: the decoded value is compared
  against a JSON string that the runtime already refuses to accept with an
  embedded NUL (`src/core/runtime.c:620`), so accepting one here could only
  produce a comparison that can never succeed.
- Decoding happens **before** comparison with the body, never after.
- A literal ASCII value that itself matches the sentinel pattern must have been
  encoded by the client. The server does not need a rule for this — it decodes
  what looks encoded — but a mismatch it produces is a genuine `HeaderMismatch`
  and is reported as one.

The comparison itself is byte-for-byte against the body value. For
`resources/read` the header is compared against the *raw* `params.uri`, not
against the canonicalized form the resources module later produces
(`docs/security-model.md:61`): normalizing before comparing would let two
different header values both pass, which is the exact ambiguity the header/body
rule exists to remove.

### `Mcp-Param-*`, and why v1 ignores it

The spec lets a server designate tool parameters for header mirroring with
`x-mcp-header` in the tool's `inputSchema`, and requires the server to validate
any `Mcp-Param-{Name}` header it *recognizes*. This runtime recognizes none: it
rejects unsupported schema keywords at registration
(`src/core/schema.c:62`, and `docs/security-model.md:43`), so no tool in this
runtime can carry an `x-mcp-header` annotation, so no `Mcp-Param-*` header is
ever expected. Unknown ones are therefore forwarded-and-ignored, which is what
RFC 9110 requires of a server that does not recognize a field.

The alternative — rejecting any `Mcp-Param-*` header outright — was considered
and refused. It would break the legitimate case of a client that talks to
several servers and sends the same annotated call to all of them, and it would
make this server's behaviour depend on a header it has no opinion about.
Supporting `x-mcp-header` properly means teaching `src/core/schema.c` a new
keyword and teaching `tools/list` to publish it, and that is a tools-module
feature, not a transport feature.

### Response modes

Two, and only two. The seam that produces them has said so since before this
document existed (`src/internal/internal.h:340`):

> A request resolves to exactly one of:
>   - a single buffered response (one `complete`, no `emit`) — what every
>     request does today over stdio, and what an HTTP `application/json`
>     reply carries;
>   - a stream (zero or more `emit` calls carrying request-scoped
>     notifications, then one `complete`) — what an HTTP `text/event-stream`
>     reply carries, and what `subscriptions/listen` needs;

**POST-only does not mean JSON-only.** The mode is chosen per request, not per
method, and not by guessing: it is decided by *what the first frame turns out
to be*. If the first frame the channel produces is the final response for this
request's id, the reply is `application/json` and that frame is the whole body.
If it is anything else — a `notifications/progress` frame, a
`notifications/subscriptions/acknowledged` frame — the reply is
`text/event-stream` and every frame including the eventual response is written
as an SSE event.

That algorithm is not invented here. It is what
`conformance/run_official_mcp.py:392` already does, and the fact that a test
adapter converged on it independently is the strongest available evidence that
it is the right one.

| Body | Reply |
|---|---|
| JSON-RPC request, first frame is the response | `200`, `Content-Type: application/json`, that response |
| JSON-RPC request, first frame is not the response | `200`, `Content-Type: text/event-stream`, SSE |
| JSON-RPC notification | `202 Accepted`, empty body |
| JSON-RPC response (has `id`, no `method`) | `400` (see below) |
| anything else | `400` |

A JSON-RPC *response* in the body is refused at the HTTP boundary rather than
passed down. The spec forbids clients from sending one, and the modern era has
no server-to-client requests for it to be answering
(`src/modules/tools.c:534`, `src/modules/resources.c:374`). Passing it down
would take it through `maelys_mcp_channel_nested_resolve`
(`src/core/channel.c:757`), which on this transport can only ever return 0, and
then through dispatch, which would answer it `Invalid Request`
(`src/core/runtime.c:612`) — into a body the client is not, by its own
protocol, supposed to be reading. `400` at the door is both cheaper and honest.

### SSE framing

```
HTTP/1.1 200 OK
Content-Type: text/event-stream
Cache-Control: no-store
X-Accel-Buffering: no
Transfer-Encoding: chunked
Connection: close

data: {"jsonrpc":"2.0","method":"notifications/progress",...}\n
\n
data: {"jsonrpc":"2.0","id":1,"result":{...}}\n
\n
```

The chunk-level framing of that body — sizes, the terminal chunk, and what
happens when a stream is abandoned — is specified under
[Chunked framing of SSE responses](#chunked-framing-of-sse-responses).

- One `data:` line per frame; the JSON is compact and contains no newline, so
  no multi-line `data:` folding is ever produced. The runtime already
  serializes compact (`src/jsonrpc/core.c:195`).
- No `event:` field and no `id:` field. There is one event type and streams are
  not resumable, so both would be noise a client is required to ignore.
- Each event is flushed before the next frame is read. A buffered SSE stream is
  a broken SSE stream, and it is the specific bug
  `conformance/run_official_mcp.py:284` documents having hit.
- The final response terminates the stream: the body ends immediately after it.
- On a long-lived stream, a bare `:` keep-alive comment is written whenever
  `maelys_mcp_channel_next` returns `MAELYS_MCP_ERR_TIMEOUT`
  (`include/maelys/mcp/channel.h:134`). The timeout is the keep-alive interval,
  so the idle path costs no extra clock and no extra thread.

### Status and error mapping

The transport chooses the HTTP status from the JSON-RPC error code of the
*response*, and it can only do so **in JSON mode** — once SSE has started, the
status line is already `200` and cannot be revised. That is sound rather than
merely convenient, because every code in the table below is produced before any
module can have emitted a frame: version and metadata validation at
`src/core/runtime.c:562`, the initialization gate at `:703`, method routing at
`:725`. A `-32601` can never arrive mid-stream.

| Condition | HTTP | JSON-RPC | Produced by |
|---|---|---|---|
| any framing violation, malformed request line or header | `400` + close | none | server layer |
| `Transfer-Encoding` present, alone or with `Content-Length` | `400` + close | none | server layer |
| `Host` absent, repeated, or mismatched | `400` + close | none | server layer |
| `Origin` present and not allowed | `403` | `-32600`, no `id` | server layer |
| header block or count over the limit | `431` + close | none | server layer |
| version other than `HTTP/1.1` | `505` + close | none | server layer |
| `Expect:` present | `417` | none | server layer |
| `Content-Length` absent on POST | `411` | none | server layer |
| `Content-Type` wrong | `415` | `-32600`, no `id` | server layer |
| `Accept` insufficient | `406` | `-32600`, no `id` | server layer |
| GET / DELETE / other method | `405` + `Allow: POST` | none | server layer |
| credential absent or invalid | `401` + `WWW-Authenticate: Bearer` | `-32600`, no `id` | authenticator |
| authenticator cannot decide | `503` | `-32600`, no `id` | authenticator |
| authenticated principal refused outright | `403` | `-32600`, no `id` | **reserved — no producer in 0.19** |
| body over the limit | `413` + close | `-32600`, no `id` | server layer |
| body not valid JSON / not a JSON object | `400` | `-32700` | adapter |
| body is a JSON-RPC response | `400` | `-32600`, no `id` | adapter |
| header missing, malformed, or ≠ body | `400` | `-32020` `HeaderMismatch` | adapter |
| unsupported protocol version | `400` | `-32022` | `src/core/runtime.c:557` |
| method not found | `404` | `-32601` | `src/core/runtime.c:725` |
| missing required client capability | `400` | `-32021` | `src/modules/mrtr.c` |
| server not initialized | `400` | `-32002` | `src/core/runtime.c:704` |
| runtime shutting down | `503` | `-32600`, no `id` | `src/core/channel.c:119` |
| policy denial on a dispatched request | `200` | `-32003` | middleware chain |
| everything else, success or error | `200` | as dispatched | modules |

> **As built: one row has no producer.** "Authenticated principal refused
> outright" describes an authenticator that accepts a credential, resolves a
> principal, and *then* refuses to serve it at all — distinct from a
> per-operation policy denial, which stays `200` + `-32003`. Neither shipped
> authenticator can do that: `loopback-trust` and `static-bearer` either
> resolve a principal or fail authentication, so every path reaching dispatch
> has an accepted principal. The row is **reserved** rather than removed,
> because the distinction it encodes is the one that keeps a transport-level
> refusal from being confused with a policy decision, and a future
> authenticator — an OAuth resource server with a revocation list, say — is
> exactly the thing that would produce it.
>
> *Resolved by the orchestrator in the owner's absence; flagged here so his
> review can override it.* The alternatives were to delete the row (losing the
> distinction) or to invent a producer for it (shipping an unused refusal
> path). Reserving it costs nothing and keeps the table's shape honest about
> what `403` is for.

The "Produced by" column is not decoration: it is the layering
([above](#layering-server-adapter-runtime)) made checkable. Everything above
the authentication rows is decided without a runtime call and without the body;
everything below `-32020` is a dispatch result the transport reports and never
rewrites. A **policy denial stays `200` with `-32003`**, because an
authenticated, well-formed request that policy refuses is a normal MCP exchange
whose answer is a refusal — identical over stdio, and not the transport's
business. HTTP `403` is reserved for refusing a principal before dispatch.

Three of these need their reasoning recorded.

**`-32020` does not exist in this codebase.** `src/internal/internal.h:27`
defines `-32600`…`-32603`, `-32002`, `-32003`, `-32021` and `-32022`, and stops
there. `HeaderMismatch` is a transport-level code with no dispatch site, so it
is added next to the others and produced only by the HTTP layer. It is the one
new protocol constant this design introduces.

**Method-not-found is `404`, not `200`.** This is the spec's rule, and its
purpose is to let a client distinguish a modern server that does not implement
a method from a legacy HTTP+SSE server that does not host this endpoint at all
— which is why the `404` must carry the JSON-RPC error body rather than an
empty one.

**`-32002` should be unreachable and is mapped anyway.** With the transport
refusing `initialize` at the door and every channel freshly created, the only
way to reach `src/core/runtime.c:703` is a request whose `_meta` lacks the
protocol-version key — which the header/body check should already have caught,
since `MCP-Protocol-Version` is mandatory and must match a value that is not
there. It is mapped because "unreachable" is a claim about two checks agreeing,
and a status table that assumes agreement hides the day they stop.

### Limits

| Limit | Default | Basis |
|---|---|---|
| request line | 8 KiB | matches the framing parser's header budget (`src/jsonrpc/core.c:14`) |
| all headers | 16 KiB | `Mcp-Name` may carry a Base64 URI, so twice the sibling budget |
| header count | 64 | |
| body | `runtime->max_message_bytes`, 1 MiB by default | `include/maelys/mcp/runtime.h:17`; the same ceiling stdio applies (`src/transport/stdio.c:137`) |
| header read | 5 s | |
| body read | 30 s | |
| JSON-mode response write | `write_timeout_ms` | reuses the stdio default (`MAELYS_MCP_DEFAULT_STDIO_WRITE_TIMEOUT_MS`) |
| SSE keep-alive | 15 s | the `channel_next` poll interval |
| SSE total duration | unbounded | a `subscriptions/listen` stream is meant to be long-lived |
| concurrent connections | 128 | |

Consistency with `max_message_bytes` is a requirement, not a coincidence: it is
the same value the channel's own output budget is derived from — `max_bytes`
defaults to `max_message_bytes * 4` (`src/core/channel.c:20`) — so a body limit
above it would admit a request whose reply the channel could refuse to queue.

## Runtime prerequisites

Two runtime changes had to land **before** the adapter could dispatch anything,
and neither was HTTP code. **Both have shipped:** the pollable outbox in 0.18.0
(#64), and the channel-context destructor with the ABI 4 config in the same
release (#63 designed it, #64 implemented it). 0.17.0 had already delivered the
third, detachable destruction (#55). This section is the record of why they
were required.

### H0a — a pollable outbox

> **As built (#64).** Shipped exactly as designed, lazily enabled, and with the
> internal visibility this section recommended: `maelys_mcp_channel_enable_wait_fd`
> and `maelys_mcp_channel_wait_fd` live in `src/internal/internal.h:832` and
> `:851`, with `maelys_mcp_outbox_enable_wait_fd` / `maelys_mcp_outbox_wait_fd`
> (`:834`, `:852`) as the layer beneath. stdio never calls either, so it still
> allocates no pipe and pays no `write(2)`.


The problem, stated exactly. `maelys_mcp_outbox_next` waits on a condition
variable (`src/core/outbox.c:371`). **A thread inside it is not watching a
socket.** Before the first frame exists that wait can last as long as the
provider takes, so a connection thread that "drains the outbox and also notices
the client left" is not describable in the current API — an earlier draft of
this document claimed it anyway, and that claim was wrong.

Three mechanisms were available.

| Option | Verdict |
|---|---|
| (a) make the outbox pollable and `poll(socket, outbox_fd)` | **chosen** |
| (b) a per-request disconnect-watcher thread | rejected |
| (c) short bounded `channel_next` waits with periodic socket checks | rejected |

**(c) is rejected** because it makes cancellation latency a tuning constant and
pays for it on every idle stream. A 50 ms poll is 20 wakeups per second per open
`subscriptions/listen` stream, forever, to detect an event that usually never
happens; a 500 ms poll keeps a provider running half a second past a disconnect
and still costs the wakeups. It is the option that works without touching the
runtime, and that is its only merit.

**(b) is rejected** because it doubles the thread count on the hot path — a
thread per in-flight request, created and joined per POST — and because it moves
the abort onto a thread that holds no reference to anything, so it needs its own
lifetime protocol against the connection thread it is racing. It converts a
missing wakeup source into a concurrency problem.

**(a) is chosen**, and three facts make it much cheaper here than it sounds.

- `maelys_mcp_outbox_t` is **opaque** — `docs/abi-policy.md:3` names it in the
  list of handles whose layout is private and will remain private. Adding a
  descriptor to it is therefore **not** an ABI break at all — unlike the era
  mask, which needs a new entry point to avoid being one.
  The only public surface is one new function, and adding a function breaks
  nothing.
- The self-pipe pattern already exists in this tree, correct and reviewed:
  `maelys_mcp_create_wakeup_pipe` (now shared, `src/core/common.c:120`) makes both ends
  `FD_CLOEXEC` and the write end `O_NONBLOCK`, and `wait_for_input_or_writer`
  (`src/transport/stdio.c:62`) already polls two descriptors and treats
  `POLLERR|POLLHUP|POLLNVAL` as a wakeup. This is the same shape, moved one
  layer down.
- `eventfd` is not used, because it is Linux-only and this project builds on
  Darwin too (`Makefile:31`). A pipe costs one extra descriptor per channel and
  works on both.

```c
/*
 * Creates this channel's wakeup pipe, so that maelys_mcp_channel_wait_fd can
 * return a descriptor. Idempotent. Called by a transport that intends to poll,
 * before it dispatches anything into the channel.
 *
 * Lazy on purpose: stdio never calls it, so stdio never allocates two
 * descriptors per channel and never pays the write() below. The cost lands
 * only on the transport that asked for it.
 */
maelys_mcp_result_t maelys_mcp_channel_enable_wait_fd(maelys_mcp_channel_t *channel);

/*
 * A descriptor that becomes readable whenever this channel's outbox has a
 * message, and stays readable until it is drained empty. Level-triggered by
 * construction: the byte is written on the empty->non-empty transition and
 * consumed when next() empties the queue, so at most one byte is ever
 * outstanding and a burst of enqueues costs one write(), not one per message.
 *
 * The caller must not read from it. It is for poll()/select()/kqueue only, and
 * the only correct response to it being readable is to call
 * maelys_mcp_channel_next(). Returns -1 when enable_wait_fd was never called
 * or failed.
 */
int maelys_mcp_channel_wait_fd(const maelys_mcp_channel_t *channel);
```

**Lazy, not automatic.** An always-on pipe would cost stdio two descriptors per
channel and a `write(2)` per idle->busy transition for a wakeup it never waits
on. `enable_wait_fd` puts that cost on the transport that benefits, and keeps
`maelys_mcp_channel_perf`'s existing figures honest for the stdio path.

**Pipe creation failure is fatal to the request, not a fallback.** If
`enable_wait_fd` fails — descriptor exhaustion, in practice — the adapter
answers `503` and does not dispatch. It explicitly does **not** fall back to
short timed waits, because that is mechanism (c), rejected above; silently
degrading into the rejected design under load is worse than failing, since the
failure is visible and the degradation would not be. The one exception is an
embedder that supplied an out-of-band cancellation entry point instead of a
descriptor (see [the flow](#the-flow)): it has its own wakeup path and needs no
pipe.

Implementation is three touched sites inside `outbox.c`, all under the mutex
that already guards the queue: raise on the transition to non-empty (next to
`pthread_cond_signal(&outbox->available)` at `src/core/outbox.c:312` and
`:252`), lower when `outbox_next` takes the last message (`:303`), and raise
unconditionally on close so a blocked poller learns the outbox is finished
(`:321`). Because the flag is maintained under that mutex, the descriptor's
state and `queued_messages` can never disagree.

The cost is one `write(2)` of one byte per idle->busy transition, not per
message — which on the dominant HTTP shape (one request, one response) is one
write per request, and on a coalescing `subscriptions/listen` stream is far
fewer than one per event. That is the number the H0 perf baseline records.

**Internal visibility for v1.** Both functions live in
`src/internal/internal.h`, not in a public header: adding a function later is
compatible, removing one is not, and nothing outside the adapter needs them
while the adapter is in the library. They become public when a second consumer
exists.

### H0b — detachable destruction: **shipped in 0.17.0**

This was the second prerequisite and it is now done, so what follows is a
record of the contract rather than a proposal. `maelys_mcp_channel_destroy`
still falls into an unbounded `pthread_cond_wait` when its bounded close misses
the deadline — correctly, for a stdio host destroying one channel at process
exit. `maelys_mcp_channel_destroy_detached`
(`include/maelys/mcp/channel.h:165`) is the entry point a channel-per-request
transport uses instead:

```
channel_destroy_detached(channel)
  ├─ closed within the deadline → freed inline, returns MAELYS_MCP_OK
  └─ deadline missed            → abort, unlink from runtime->channels,
                                  detached_channel_count++, mark detached,
                                  return MAELYS_MCP_ERR_TIMEOUT immediately.
                                  Whichever in-flight operation finishes last
                                  performs the real free on its own thread.
```

Two properties the shipped implementation has that this document had asked for,
and one it has that this document had missed:

- The free runs through `free_channel_storage` (`src/core/channel.c:260`),
  which has exactly two callers — the detached path (`src/core/channel.c:299`)
  and the synchronous one (`src/core/channel.c:299`) — so there is a single
  place where a channel's storage goes away whichever route it took.
- `maelys_mcp_runtime_destroy` drains `detached_channel_count`
  (`src/core/runtime.c:161`) before it proceeds, so a detached channel can
  never outlive the runtime it points at.
- The missed piece was **the context**. Detached destruction frees the channel
  but has no way to release what `config.context` designates; the header says
  so and tells the embedder to wait for `runtime_destroy`
  (`include/maelys/mcp/channel.h:177`). For a per-request transport that is a
  leak with a schedule, and closing it is the `context_release` callback in
  PR #63, which shipped in 0.18.0 (#63, #64).

## Internal architecture

### The flow

```
POST /mcp                                        ── the server layer (host/) ──
  ├─ parse request line + headers          bounded; strict; close on any error
  ├─ Host                                  → 400
  ├─ Origin                                → 403
  ├─ route path → this adapter             → 404 (no MCP body)
  ├─ method is POST                        → 405 + Allow
  ├─ Expect / framing / media              → 417 / 411 / 413 / 415 / 406
  ├─ build credentials FROM HEADERS ONLY
  ├─ authenticate(credentials, &principal) → 401 + WWW-Authenticate / 503
  │     on failure: reply, shutdown(SHUT_WR), discard ≤8 KiB, close.
  │     The body is never read at full size for an unauthenticated caller.
  ├─ read body                             bounded by Content-Length
  │                                        ── the adapter (library) ──
  ├─ parse JSON, classify request/notification/response
  ├─ header ↔ body validation              → 400 -32020
  ├─ channel_create(context = principal, protocol_eras = MODERN)
  ├─ channel_accept(channel, message)
  ├─ poll{ cancel_fd, shutdown_fd, channel_wait_fd }
  │        ├─ first frame is the response  → 200 application/json
  │        └─ otherwise                    → 200 text/event-stream
  ├─ channel_close(deadline) ; drain to ERR_CLOSED ; end_stream(COMPLETE)
  └─ channel_destroy_detached ; context_release runs at the real free
```

**Authentication precedes the body read**, which is the ordering
`docs/authenticated-principal-design.md` makes a contract: an unauthenticated
caller can otherwise hold a thread for the body deadline and push a
body-limit-sized allocation. It also precedes channel creation, because the
principal is a channel *creation* parameter and there is no write API for it.

The header↔body validation necessarily follows the body read — it compares the
two — so it sits on the adapter side of the line, after authentication. That
ordering has a second benefit worth naming: `-32020` is only ever produced for
a caller the server has already identified, so header-mismatch telemetry is
attributable rather than anonymous.

> **As built.** The ladder above is the shipped order
> (`host/http_server.c:736`), and five things the design left implicit were
> settled while building it.
>
> **Method precedes the media checks**, which the design's diagram had the
> other way round. A `GET` carries neither `Content-Type` nor `Accept`, so
> checking media first would answer a `GET` with `415` when the methods table
> promises `405`. Path routing (`404`) and then method (`405`) come first, and
> only a POST to this endpoint is ever asked about its media types
> (`host/http_server.c:728`).
>
> **`Origin` still runs first among the security checks** — it is the
> DNS-rebinding control and must precede the body, authentication, and any
> channel — with `Host` immediately before it, because `Host` is what `Origin`
> and the absolute-form target are validated *against*
> (`host/http_server.c:742`).
>
> **`400` is for a syntactic `Content-Length`, `413` for a well-formed one over
> the limit.** The design's limits table implied only that a body over the
> limit is refused; the parser distinguishes a value that is not `1*DIGIT` (a
> framing fault, `400`, connection closed) from a valid length exceeding
> `max_body_bytes` (`413`, `host/http_server.c:775`). They are different faults
> and a client can act on only one of them.
>
> **The RST-safe close is not the `401`'s alone.** The principal document
> introduced write-then-`shutdown(SHUT_WR)`-then-bounded-discard for
> authentication failures; as built it covers **every** rejection that happens
> while a client may still be writing, `413` and `415` included, "the hazard is
> the same" (`host/http_server.c:868`). The design under-scoped it to the one
> case it happened to be describing.
>
> **The status table is normative over the writer's comment.** `status_only`'s
> doc comment lists example statuses; where the two disagree, the
> [status mapping](#status-and-error-mapping) table governs, and the header was
> reworded to stop enumerating (`include/maelys/mcp/http.h:161`).

### Channel per request

`maelys_mcp_channel_accept` is the seam
(`src/core/channel.c:746`), and it was built for this. Its own header comment
says so: it is "the seam a transport calls instead of
`maelys_mcp_channel_handle`, and the reason `stdio.c` is a thin adapter"
(`src/internal/internal.h:737`). `stdio.c` calls it in exactly one place
(`src/transport/stdio.c:254`) and comments that handing the frame to the
channel "is what makes this loop reusable by a transport that is not stdio"
(`src/transport/stdio.c:252`). HTTP is the transport that comment was written
for.

One channel per POST, created and destroyed inside the request. That has three
consequences worth stating rather than discovering.

**It is what makes the era rule structural** — see [The era rule](#the-era-rule-and-the-per-channel-version-mask).

**It puts channel create/destroy on the hot path, and that path is globally
serialized.** `publish_channel` takes `lifecycle_mutex` and then
`channels_mutex` (`src/core/channel.c:141`, `:164`); `channel_destroy` takes
`channels_mutex` to unlink (`src/core/channel.c:1055`). Both are runtime-wide.
This is precisely why `tests/test_channel_perf.c` exists, and its header
comment already names the consumer: "Every create and destroy takes
`lifecycle_mutex` and `channels_mutex`, so this is a global serialisation
point; the numbers say how much headroom a per-request-channel transport
(HTTP) has" (`tests/test_channel_perf.c:8`). The HTTP baselines required by
H4 extend that file rather than inventing a new pattern, and its loose
`CEILING_US` discipline (`tests/test_channel_perf.c:36`) is kept for the same
reason it was chosen: a tight timing gate in CI is a flakiness source.

**It costs a `pthread_create` per `tools/call`, and the cost buys something
other than what it was built for.** `channel_accept` offloads `tools/call` and
`resources/read` to a worker thread and deep-copies the request tree for it
(`src/core/channel.c:761`, `:652`). It was built so the transport's reader
could stay free to carry a nested reply back — and on this transport there are
no nested replies, ever (`src/modules/tools.c:550`,
`src/modules/resources.c:384`), so that justification does not apply.

The offload is kept anyway, for a different and equally load-bearing reason:
**the connection thread must stay out of the dispatch so that it is free to sit
in `poll()`**, watching the socket for the client's FIN alongside the outbox
descriptor from H0a. Dispatching inline would put the only thread that can
observe a disconnect inside the call that the disconnect is supposed to cancel.

Being precise about what each half contributes, because an earlier draft
conflated them and the conflation is what made its cancellation chain
unrealizable: the offload is what stops the connection thread being *inside the
provider call*; the pollable outbox is what stops it being *blocked on a
condition variable*. Neither alone is sufficient — without H0a the connection
thread would be out of the dispatch and still blind, parked in
`maelys_mcp_channel_next` until a frame it may never receive. Together they are
exactly what "poll the socket and the outbox at once" requires, and no
per-transport branch is introduced to get it.

The side effect is that `channel->transport_demuxes` becomes 1
(`src/core/channel.c:701`) and dispatch therefore sets `nestable = 1`
(`src/core/runtime.c:658`). That is inert: the relay is gated on
`!request->modern` first (`src/modules/tools.c:550`), and on this transport
`modern` is always true.

### Threading model, against the existing hierarchy

```
acceptor thread   accept(), hand the connection to a connection thread
                  (server layer)
connection thread parse, Host/Origin/route/method/media, authenticate,
                  read body                                 (server layer)
                  then, inside maelys_mcp_http_adapter_handle:
                  channel_create, channel_accept,
                  poll{ cancel_fd, shutdown_fd, channel_wait_fd }
                    → channel_next → write, close, destroy-detached (adapter)
watcher thread    one per exchange, server layer only: poll() on the SOCKET,
                  MSG_PEEK to tell FIN from a pipelined byte, then write one
                  byte to a pipe. Names nothing, owns nothing, aborts nothing.
worker thread     dispatch of tools/call / resources/read, owned by the channel
reaper            no thread of its own: a detached channel is freed by
                  whichever worker releases the last reference (H0b)
```

The lock hierarchy in `src/internal/internal.h:103` — `lifecycle_mutex` →
`channels_mutex` → `channel->mutex` — is **not extended by the transport**. The
connection thread touches those locks only through the existing public calls
(`channel_create`, `channel_next`, `channel_close`, `channel_destroy`), and it
holds none of them across a `write()`. That is the same discipline the
hierarchy comment already demands: "Blocking activation, enqueue, and I/O run
without the lifecycle, registry, or channel metadata locks held"
(`src/internal/internal.h:107`).

The two H0 prerequisites are the only things that touch the hierarchy at all,
and both were designed to avoid adding an edge:

- **H0a adds no lock.** The wakeup flag lives inside `outbox->mutex`, which is
  already the innermost lock on that path and is never held across the
  one-byte `write()` — the descriptor is raised under the mutex and the write
  is issued after it is released, exactly as `emit_event_to_channel` already
  releases `channel->mutex` before enqueuing (`src/modules/subscriptions.c:385`).
- **H0b takes `channels_mutex` then `channel->mutex` to unlink**, which is the
  documented order and the same pair `channel_close` already takes in that
  sequence (`src/core/channel.c:912`). The free itself runs with no runtime
  lock held, from a thread that has just released its own reference.

The transport's own state — connection count, listener socket, shutdown flag —
lives behind a single `http->mutex` that is **never** held while any runtime
call is in progress. It is therefore a leaf, and adds no edge to the hierarchy
above. Stating it as a leaf is the whole of the concurrency review this
transport needs, and it is a property H4's TSan run checks rather than assumes.

A connection thread per connection, not a thread pool, in v1. The concurrency
bound is the connection limit. A pool is a later optimisation whose value
depends on measurements that do not exist yet; introducing one now would mean
designing a queue whose depth nobody can justify.

### The sink: the channel's own, not a bespoke one

`src/internal/internal.h:731` says a transport that delivers a request's
response itself "passes its own" sink. **This design does not take that
option**, and the reason is the single most consequential call in the document.

A bespoke HTTP sink would receive `emit` and `complete` for one request and
write them straight to the socket. It works for `tools/call` with progress. It
fails completely for `subscriptions/listen`, because subscription fanout does
not go through the sink at all: `emit_event_to_channel` enqueues directly into
the channel's outbox (`src/modules/subscriptions.c:397`), and that bypass is a
named hole in the middleware design (`docs/middleware-design.md:357`), not an
oversight to route around. A bespoke sink would see the acknowledgement and
then nothing, forever.

So the HTTP transport uses `maelys_mcp_channel_outbox_sink`
(`src/core/channel.c:424`) — the same sink stdio uses — and its connection
thread plays the role stdio's writer thread plays, draining
`maelys_mcp_channel_next` (`src/core/channel.c:833`) and writing what comes
out. Everything converges on one ordered queue: progress frames, the
acknowledgement, subscription fanout, and the final response.

Four properties follow, and each of them is why this is the right call:

- **Ordering is already correct, and for the SSE reason specifically.**
  `outbox_sink_emit` classifies request-scoped notifications as
  `MAELYS_MCP_OUTBOX_RESPONSE` rather than `NOTIFICATION`, and the comment
  explaining why is about this transport: `select_next` prefers responses over
  notifications, "which would let a request's final response overtake the
  request-scoped notifications that must precede it. Over SSE that is not
  merely out of order: the final response terminates the stream, so those
  notifications would be dropped outright" (`src/core/channel.c:398`). The
  ordering guarantee this transport needs was designed into the outbox before
  the transport existed.
- **Fanout coalescing keeps working.** Subscription events are enqueued as
  `NOTIFICATION` with a coalescence key (`src/modules/subscriptions.c:397`), so
  a slow SSE client collapses duplicate invalidations instead of accumulating
  them. A bespoke sink would have had to reimplement that.
- **Backpressure keeps working.** The outbox is bounded and admission is
  timed; a client that stops reading eventually faults its own channel through
  `fault_on_timeout` (`src/core/channel.c:370`) rather than growing a buffer.
- **`wrap_sink` keeps working, untouched.** ⑥ wraps whatever base sink dispatch
  is handed (`src/core/channel.c:474`), and every guarantee `docs/middleware.md`
  makes about it — `complete` forwarded exactly once, a swallowed completion
  detected and answered past the chain (`src/core/channel.c:505`), wrappers
  released in reverse order — is expressed against the sink chain, not against
  the transport underneath it. Because this transport passes the same base sink
  stdio passes, ⑥ behaves identically on both, and the middleware suite needs
  no HTTP-specific case to prove it.

The cost is one extra hop: a frame is enqueued, then dequeued, rather than
written directly. On a `tools/call` with no progress that is one queue
operation on a single-element queue. The perf baseline in H4 measures
it; the design's position is that it is worth paying for a transport with one
delivery path instead of two.

### `subscriptions/listen`

A long-lived **request-scoped SSE response**. Not a session — a single HTTP
request that stays open. The channel lives for exactly as long as that request,
and there is no state on either side that outlives it.

```
POST subscriptions/listen           adapter creates a channel (A-chain)
  → dispatch returns the ack        src/modules/subscriptions.c:303
  → complete(ack)                   ⇒ outbox, RESPONSE class
  → first frame is NOT this id's response ⇒ SSE mode, begin_stream()
  → write_event: notifications/subscriptions/acknowledged
  → ... poll{ cancel_fd, shutdown_fd, channel_wait_fd }, keepalive_interval_ms
      nothing ready ⇒ write_keepalive()   (":" comment)
      wait_fd ready ⇒ channel_next() ⇒ write_event()
      cancel_fd     ⇒ A2..A6: abort, end_stream(ABORTED), NO terminal chunk
      shutdown_fd   ⇒ exchange_shutdown: complete every subscription with
                      resultType:"complete", write those frames,
                      end_stream(COMPLETE) — the peer is owed an ending
  → channel_destroy_detached; context_release at the real free
```

The two endings are **different signals and not synonyms**, which the design's
earlier diagram blurred by routing both through "client closes". A cancelled
listen stream ends truncated because the peer is gone; a shut-down one is
completed properly because the peer is still there and reading
(`include/maelys/mcp/http.h:119`).

The single fact that makes the mode selection work here is one most readers
guess wrong: **`listen` returns the acknowledgement as its dispatch
"response"** (`src/modules/subscriptions.c:303`), so it travels through
`sink->complete` (`src/core/channel.c:502`) even though it is a JSON-RPC
*notification* and carries no `id`. The transport's "is this frame the final
response for this request's id?" test therefore says no — correctly — and the
stream opens. `complete` is not the end of the exchange on this method, and any
transport that assumed it was would close the stream on the acknowledgement.
`docs/subscriptions.md:20` states the protocol half of this ("The listen
request then remains open; it does not receive an immediate JSON-RPC result");
the implementation half is that the acknowledgement still travels the
completion path.

At close, `channel_close` calls `maelys_mcp_channel_complete_subscriptions_until`
(`src/core/channel.c:941`), which enqueues one `resultType: "complete"`
response per surviving id (`src/modules/subscriptions.c:517`). The connection
thread must therefore keep draining *after* it has asked for the close, until
`channel_next` returns `MAELYS_MCP_ERR_CLOSED`, or that final frame is written
to a socket nobody will read. This is the shutdown ordering that
`docs/subscriptions.md:50` describes from the runtime's side.

Cross-channel fanout is unaffected by the short-lived channels around it.
`maelys_mcp_runtime_snapshot_channels` targets every active channel
(`src/core/channel.c:841`), but `emit_event_to_channel` filters by subscription
first (`src/modules/subscriptions.c:376`), so a `tools/call` channel that will
live for four milliseconds and never subscribed receives nothing. What it does
cost is a walk over a channel list whose length is now "in-flight requests"
rather than "connected clients" — measurable, and measured in H4.

### Cancellation

On this transport the client closing the SSE stream **is** the cancellation.
Modern HTTP clients do not send `notifications/cancelled`; that message is
stdio-only in this revision. The runtime still handles it if one arrives
(`src/core/runtime.c:634`), and a POST carrying it is answered `202` — but it
is not the mechanism, and the design does not rely on it.

The chain has two halves that meet at one abstract fact, and keeping them apart
is what lets the adapter stay socket-free. **The server layer decides that a
cancellation happened; the adapter reacts to it.**

*Server layer — the only place that knows about sockets:*

```
S0. a WATCHER THREAD owns the socket while the connection thread runs the
    exchange, because the two jobs cannot share a thread (see below).
S1. client closes → POLLIN | POLLHUP | POLLERR on socket_fd
S2. recv(socket_fd, &b, 1, MSG_PEEK) disambiguates:
        0  → FIN: the client is gone
       >0  → inbound bytes mid-exchange, which the pipelining rule forbids:
             a protocol error, logged as one, same teardown
S3. either way: make the exchange's cancel_fd readable
    (or call maelys_mcp_http_exchange_cancel for an embedder without one)
```

*Adapter — the same code whatever the cause was:*

```
A0. blocked in poll({ cancel_fd, POLLIN },
                    { maelys_mcp_channel_wait_fd(ch), POLLIN })
    - simultaneously waiting for output and for cancellation.
A1. cancel_fd readable ⇒ this exchange is cancelled. No further interpretation.
A2. maelys_mcp_channel_abort(channel)                src/core/channel.c:355
      └─ fault_channel: state = FAULTED, targetable = 0  src/core/channel.c:340
      └─ nested_fail_all_locked (no-op on this transport) src/core/channel.c:349
      └─ outbox_close(discard = 1)                       src/core/channel.c:358
           └─ which also raises the wait fd, so a concurrent poller wakes
A3. sink->cancelled() now returns 1                   src/core/channel.c:416
      └─ a provider polling the progress reporter sees it and may return early
A4. every subsequent emit/complete fails ERR_CLOSED    src/core/channel.c:361
      └─ so "MUST NOT send any further messages for it" holds by construction,
         not by discipline
A5. the worker's dispatch fails, records dispatch_status, aborts again
                                                       src/core/channel.c:604
A6. end_stream(MAELYS_MCP_HTTP_STREAM_ABORTED) — no terminal chunk
A7. channel_destroy_detached(channel)         include/maelys/mcp/channel.h:165
      ├─ OK        → freed inline; context_release already ran
      └─ TIMEOUT   → detached; the last worker out frees it and runs
                     context_release then
A8. the connection and its slot are released in bounded time either way.
```

`MSG_PEEK` at S2 is what makes the disconnect test exact rather than heuristic:
`POLLIN` fires for both "peer sent data" and "peer sent FIN", and a server that
treated the first as a disconnect would abort live requests whenever a client
pipelined. Because [pipelining is forbidden](#keep-alive-and-pipelining) both
cases end in the same teardown — but they are *different faults* and are logged
differently, and conflating them at the source would hide a client bug behind a
cancellation.

> **As built: S0 is a separate thread per exchange, and that is not option (b)
> resurrected.** The design's S-chain said "the connection thread … is the only
> party watching it", which cannot work: the connection thread is inside
> `maelys_mcp_http_adapter_handle`, and the adapter's drain sits in `poll()` on
> descriptors that are deliberately **not** sockets. So the server layer starts
> a small watcher thread alongside the exchange (`host/http_server.c:544`).
>
> This must be reconciled with [H0a](#h0a--a-pollable-outbox), which rejected
> "(b) a per-request disconnect-watcher thread". **The rejection stands and
> this does not contradict it, because they answer different questions.** H0a
> was choosing a mechanism for *waking a thread blocked on the outbox*, and
> rejected a watcher there because a pollable outbox makes the wakeup a
> descriptor and a thread would have been a worse way to get the same signal —
> "it converts a missing wakeup source into a concurrency problem". The watcher
> that shipped does no waking of that kind and touches no outbox: it performs
> **socket disambiguation**, the one job H0a's descriptor cannot do and the
> adapter must not do. Choosing the abstract-cancellation seam
> ([Correction 2](#where-the-code-contradicted-the-drafts)) is precisely what
> makes it necessary — somebody has to turn "a byte arrived on this socket"
> into "this exchange is cancelled", and it cannot be the layer that is
> forbidden to know what a socket is.
>
> The concurrency objection H0a raised against option (b) is answered rather
> than ignored, and the source says how (`host/http_server.c:529`): the
> lifetime protocol "is a pipe rather than a reference, which is what makes it
> small: this thread never names the exchange, never touches the channel, and
> never aborts anything. It writes one byte." The connection thread creates the
> pipe before the watcher exists and closes it after joining, "so there is no
> window in which this thread holds a descriptor number that has been handed to
> something else". A watcher that owns nothing and names nothing has no
> lifetime protocol to get wrong, which is exactly the property option (b)
> lacked when it was proposed as an outbox-wakeup mechanism.

That distinction is precisely why it lives at S2 and not in the adapter. It
needs the socket, it needs the pipelining rule, and it needs to know which of
the two happened. The adapter needs none of those things — it needs to stop —
so it is told only that, and an embedder with no socket at all can produce the
same signal.

Step 4 is the honest weak point and must be stated as one. `sink->cancelled` is
a **poll**, not an interrupt: `outbox_sink_cancelled` reports `channel->state
!= ACTIVE` (`src/core/channel.c:419`), and a provider that never asks never
learns. A process provider blocked in its call round trip is bounded by
`call_timeout_ms` (`src/internal/internal.h:461`) and no sooner; an in-process
provider that ignores the reporter is bounded by nothing.

That asymmetry is not this document's to close, and the sibling design confirms
it rather than contradicting it: `docs/launch-contract-design.md` removes the
unbounded waits from process-provider teardown (`docs/launch-contract-design.md:839`)
and explicitly declines to cover in-process providers, which are "inside the
trust boundary by definition" (`docs/launch-contract-design.md:1135`). So the
process half gets tighter while this design is being reviewed, and the
in-process half stays exactly as stated here — which is the half
[H0b](#h0b--detachable-destruction-shipped-in-0170) exists to make survivable, since a bound
that does not exist cannot be waited on.

So the guarantee this design makes is precise and smaller than "the work
stops":

> After a client disconnect, no further bytes are written for that request, no
> further frame can be admitted to its channel, and the channel is destroyed
> once its in-flight work returns. Whether the *provider* stops is the
> provider's own timeout, and the runtime does not shorten it.

That matches the spec, which says a server **SHOULD** stop work as soon as
practical and **MUST NOT** send further messages: the MUST is enforced
structurally at step 5, the SHOULD is delegated where the ability actually
lives. Claiming more would be claiming a provider-cancellation mechanism that
does not exist.

Step 7 is where the guarantee is actually earned, and it is the reason
[H0b](#h0b--detachable-destruction-shipped-in-0170) is a prerequisite rather than a nicety.
`channel_close` is bounded by its deadline (`src/core/channel.c:925`); today
`channel_destroy` is not, falling into an unbounded `pthread_cond_wait` on
`operations_inflight` (`src/core/channel.c:1033`) whenever that deadline is
missed. Detaching converts that from "the connection thread waits as long as
the provider takes" into "the channel outlives the connection and is freed by
its last worker" — which is the only shape that is both memory-safe and
slot-bounded. The unbounded wait does not disappear; it stops being something a
network peer can hold.

### Shutdown

Server shutdown is three ordered phases, and the order is the whole content:

1. Stop accepting: close the listener, refuse new connections. In-flight
   requests are untouched.
2. Ask every open SSE stream to finish: `channel_close(deadline)` on each,
   which triggers the `resultType: "complete"` responses
   (`src/core/channel.c:941`). Keep draining each until `ERR_CLOSED`, write the
   terminal chunk, then end the body. A stream whose close misses the deadline
   is detached per H0b rather than waited on.
3. Join every connection thread. Every one of them terminates in bounded time,
   because step 2 gave each a deadline and detaching is what happens when the
   deadline is missed.
4. `maelys_mcp_runtime_destroy`, which drains `detached_channel_count` before
   taking `lifecycle_mutex` — the H0b invariant that stops a detached channel
   from outliving the runtime it points at.

Runtime destruction already closes and drains the channel-create gate before
taking `lifecycle_mutex` (`src/internal/internal.h:109`), so a connection thread
racing to create a channel during phase 1 gets `MAELYS_MCP_ERR_STATE` from
`begin_channel_create` (`src/core/channel.c:119`) rather than a torn runtime.
The transport maps that to `503 Service Unavailable`, which is the one status
in this design that is not in the spec's table and is right anyway.

## The parser, and its fuzz targets

Hand-rolled and incremental. The argument is **not** zero dependencies — the
project already links jansson and uriparser (`Makefile:15`), so that claim would
be false. The argument is that the parser is **deliberately small and heavily
fuzzed**: an HTTP/1.1 request-line-and-header reader that accepts one method and
one framing is a few hundred reviewable lines, whereas a general HTTP library is
a large attack surface bought for features this endpoint refuses on purpose.

`src/jsonrpc/core.c` is the model to copy rather than a thing to reuse. Copy:
the append/consume ring (`src/jsonrpc/core.c:17`, `:38`), the CRLFCRLF
terminator scan (`:52`), the overflow-checked decimal parse (`:61`), and the
duplicate-header refusal (`:95`). Do not reuse: its `parse_frames` is an LSP
`Content-Length` framer with no request line, no method, and no header table,
and bending it into an HTTP parser would make one function serve two protocols —
the failure mode this codebase spends most of its structure avoiding.

### Strict rejection rules

Every one of these is a `400` (unless noted) followed by **an unconditional
connection close**. Nothing is ever resynchronized: a parser that tries to find
the next request after a framing error is the parser that gets smuggled through.

*Framing and the smuggling class.*

- `Transfer-Encoding` present at all → refuse. Not merely `chunked`: any value,
  any casing, any list, including `identity` and the classic
  `chunked, chunked` and `chunked\r\nTransfer-Encoding: identity` obfuscations.
- `Transfer-Encoding` **and** `Content-Length` both present → refuse, and log
  it as a smuggling attempt rather than as a malformed request. This is the
  single most exploited desync primitive and it deserves its own signal even
  though the previous rule already covers it.
- `Content-Length` repeated → refuse, even when the values agree. Agreeing
  duplicates are what a front end and a back end disagree about later.
- `Content-Length` value that is not exactly `1*DIGIT` → refuse: no leading `+`,
  no sign, no leading or trailing space, no hex, no `0x`, no empty value.
  Overflow is refused rather than truncated (`src/jsonrpc/core.c:76` is the
  arithmetic to copy).
- Absent `Content-Length` on a POST → `411 Length Required`. There is no
  implicit zero-length body.

*Request line and headers.*

- Bare LF as a line terminator anywhere → refuse. CRLF only.
- Any whitespace before the request line, including a leading CRLF → refuse.
  The RFC's tolerance for a leading empty line is a resynchronization aid and
  this parser does not resynchronize.
- `obs-fold` continuation lines (a header line starting with SP or HTAB) →
  refuse.
- Whitespace between a field name and its colon → refuse.
- A header name containing anything outside RFC 9110 `tchar` → refuse.
- Any header value byte that is NUL, CR or LF → refuse, before the value is
  interpreted by anything.
- HTTP/0.9, or any version other than `HTTP/1.1` → `505 HTTP Version Not
  Supported`. `HTTP/1.0` is refused rather than downgraded to, because its
  connection semantics are a second state machine.
- An absolute-form request target whose authority does not match `Host`, or
  whose path does not match the configured endpoint → refuse. `*` and
  authority-form targets → refuse.
- Percent-encoded path traversal, `..` segments, or a NUL in the target →
  refuse. The path is compared literally against the configured endpoint after
  a single normalization pass; a target that needs more than that is not this
  endpoint.
- Request line over its limit, header block over its limit, or header count
  over its limit → `431 Request Header Fields Too Large`.

*Expectations.*

- `Expect:` with any value → `417 Expectation Failed`. Worth one sentence of
  honesty: `100-continue` is the *standard* solution to the very problem
  ["authenticate before the body"](#the-flow) creates — it exists so a server
  can answer `401` before the client sends a byte of body. Supporting it is
  therefore the natural refinement rather than a gap, and it is out of v1 only
  because it adds a second inbound state to a parser whose smallness is the
  security argument. The bounded discard described in the principal document
  covers the same ground less elegantly and with no new parser state.

### Keep-alive and pipelining

Decided, and the two halves are one decision:

- **Pipelining is never permitted.** Any inbound byte arriving on the
  connection between the end of a request's headers-plus-body and the
  completion of its response is a protocol error: the connection is closed and,
  if a response is in flight, it is abandoned. This is not only a smuggling
  defence — it is what makes the disconnect detector sound, because it lets
  "readable socket" mean "the peer went away" rather than "the peer went away
  *or* sent something".
- **Keep-alive is permitted for JSON-mode responses and refused for SSE.** A
  JSON reply leaves the connection in a known state and reuse saves a handshake
  on the hot path, which matters on a transport that opens a connection per
  request. An SSE reply is answered with `Connection: close` and the connection
  is closed after the terminal chunk: reuse after a long-lived stream saves
  nothing measurable and asks the framing state machine to be right about a
  case that would otherwise never be exercised.
- Each request on a reused connection is authenticated independently
  (`docs/authenticated-principal-design.md`), so keep-alive is a transport
  optimisation and never a session.
- `Connection: close` from the client is honoured. Idle keep-alive connections
  are closed after 5 s and count against the connection limit until then.

### Chunked framing of SSE responses

The SSE body is `Transfer-Encoding: chunked` on the **response** side — refused
inbound, required outbound, and there is no contradiction: the danger is
ambiguity about what a *request* body is, and a response the server itself
frames has no such ambiguity.

```
<hex-size>\r\n
data: {json}\n
\n
\r\n
...
0\r\n
\r\n
```

- One chunk per SSE event. The chunk size is the length of
  `"data: " + json + "\n\n"` in lowercase hex with no extensions, and the chunk
  data is followed by CRLF.
- Every chunk is flushed before the next frame is read from the outbox.
- A keep-alive comment is its own chunk carrying `:\r\n`.
A stream ends in exactly one of two ways, and `end_stream`'s `disposition`
argument selects which. This is the whole of the contract that an earlier
revision left self-contradictory — it asked for `end_stream` "exactly once"
*and* for an abandoned stream to close without a terminal chunk, which are
incompatible if `end_stream` always writes one.

**`MAELYS_MCP_HTTP_STREAM_COMPLETE`** — the final JSON-RPC response was
delivered. The writer emits the terminal chunk and nothing else:

```
30\r\n                                   <- last event's chunk
data: {"jsonrpc":"2.0","id":1,...}\n\n
\r\n
0\r\n                                    <- terminal chunk
\r\n                                    <- no trailers
```

The connection is then closed, because [SSE replies are not
reused](#keep-alive-and-pipelining).

**`MAELYS_MCP_HTTP_STREAM_ABORTED`** — the exchange was cancelled, a write
failed, or the chain faulted. The writer emits **no further bytes at all** and
closes the underlying connection:

```
30\r\n                                   <- whatever chunk was last written
data: {"jsonrpc":"2.0","method":"notifications/progress",...}\n\n
\r\n
                                        <- nothing. connection closed here.
```

No `0\r\n`, no trailers, no partial chunk completed. A chunked body that ends
without its terminal chunk is exactly how HTTP/1.1 says "this response is
truncated", and every conforming client treats it as an error rather than as a
short but valid answer. Writing `0\r\n\r\n` and then closing would instead
tell the client the stream completed normally — the one lie a truncated
response must not tell, and one that would silently turn a cancelled
`tools/call` into an apparently-successful empty one.

`end_stream` is still called exactly once per `begin_stream`; what varies is
what it writes. An implementation that has already written a partial chunk when
it aborts does not attempt to finish it.

The COMPLETE shape is exactly what `conformance/run_official_mcp.py:418`
already emits, which is the second place that adapter turns out to have
prototyped the right answer.

### Fuzz targets

The repository builds **five** libFuzzer targets today — `json-lines`,
`content-length`, `schema`, `content`, `uri` (`Makefile:415`) — each wired the
same way, with a corpus seeded in `fuzz-smoke` and run for 2000 iterations in
CI (`Makefile:472`). New targets dedicated to the HTTP boundary follow that
pattern exactly, and there are several because the boundary has several
independently reachable state machines:

| Target | Entry point | What it must not do |
|---|---|---|
| `http-request` | arbitrary bytes in random chunk sizes into the request-line and header parser | over-read, over-allocate, accept a malformed request line, disagree with itself across chunk boundaries |
| `http-smuggling` | a generated header block drawn from a grammar that *biases toward* framing ambiguity | accept any `Transfer-Encoding`, accept TE+CL together, accept duplicate or non-`1*DIGIT` `Content-Length`, accept obs-fold, accept bare LF, ever report a body length two readers could disagree about |
| `http-headers` | a synthesized header block + a body, into name normalization, sentinel decoding and the header↔body comparison | accept a mismatch, crash on a truncated or over-padded sentinel, mis-decode a value containing the sentinel pattern |
| `http-exchange` | a whole exchange script — headers, body, and a scripted disconnect at an arbitrary byte offset — driven through `maelys_mcp_http_adapter_handle` with a recording writer | write after cancellation, emit a second `complete`, write a terminal chunk on an abandoned stream, leak a channel or a principal, deadlock |
| `http-origin` | `Origin`, `Host` and `Mcp-Name` values against the validators and the sentinel decoder | accept a disallowed origin, accept a mismatched or duplicated `Host`, accept a NUL or CRLF, accept invalid UTF-8 out of the decoder |

**`http-smuggling` is a required target, not an optional one**, and it is
separate from `http-request` on purpose: a uniformly random corpus almost never
produces two framing headers that disagree, so the ambiguity class has to be
generated deliberately rather than hoped for. Its grammar emits `Content-Length`
and `Transfer-Encoding` in every combination of casing, repetition, list form,
whitespace, and obs-fold, and its invariant is the strong one — **the parser
must return a single unambiguous body length or refuse**, never a length that
depends on which header it happened to read last.

The chunk-boundary property is the one worth calling out for the others,
because it is the class of bug an incremental parser is *for*: feeding the same
input in one chunk and in random chunks must produce the same decision, and the
existing `content-length` target already fuzzes exactly that shape
(`fuzz/fuzz_content_length.c:22`). `http-exchange` needs a stub channel rather
than a real runtime, so an iteration costs no provider and no thread — which is
the payoff of the [adapter seam](#layering-server-adapter-runtime) being a
function rather than a socket.

These land in **H1** (`http-request`, `http-smuggling`, `http-origin`), **H2**
(`http-headers`) and **H3** (`http-exchange`) — with the code they cover, never
after it.

## Security model delta

> **As built: every bullet below has landed**, across H1–H4, under **Defaults**
> in `docs/security-model.md:79`–`:130`. The drafted text is kept as written
> because it is the record of what the transport was *required* to guarantee
> before it existed, and the shipped bullets can be read against it.
>
> **One paragraph of `docs/security-model.md` is now stale, and it is not this
> document's to fix.** Its trust-boundary section still says "The runtime
> serves MCP over stdio only … it does not dispatch, so no MCP request has yet
> reached a provider over HTTP", and names "phases H2 and H3" as the remaining
> work (`docs/security-model.md:185`). H2 (#69) and H3 (#70) shipped; the
> listener dispatches. That paragraph was accurate when H1 landed and was
> overtaken twice since. It is flagged rather than edited here because this
> change is scoped to one file and a security statement deserves its own
> reviewed commit — but it is a security document currently asserting something
> false about network exposure, which is the most expensive kind of staleness
> this tree can carry.
>
> Its *substance* still needs carrying over when it is rewritten: the effect
> policy remains runtime-wide rather than per-principal, and no middleware in
> this repository reads the channel-bound principal yet
> (`docs/security-model.md:193`). "The listener serves MCP" is done; "the policy
> decides on the principal" is not.

The section below was drafted for `docs/security-model.md` as it would read
once this shipped. Under **Defaults**, after the existing stdio-write bullet:

> - The HTTP transport serves `2026-07-28` only. Legacy protocol support stays
>   stdio-only, and a channel exists for exactly one request, so no legacy
>   session state can be established over HTTP even if a client tries.
>   `initialize` and `notifications/initialized` are refused at the HTTP
>   boundary rather than dispatched.
> - The HTTP transport binds `127.0.0.1` by default. Binding any other address
>   requires an explicit flag and refuses to start without an authenticator
>   other than loopback-trust.
> - `Origin` is validated on every request. When present and not in the
>   configured allowlist the request is refused with `403` before the body is
>   read, before authentication, and before any channel exists. The allowlist
>   is empty by default; a request with no `Origin` is accepted only on a
>   loopback bind. This is the DNS-rebinding control, and it is the one check
>   that runs earliest.
> - The transport principal is established at channel creation from a
>   credential the transport itself authenticated, and is bound to the
>   channel's opaque context. It is never derived from payload metadata.
>   `clientInfo.name`, `_meta`, and the client-written `Mcp-Method`,
>   `Mcp-Name` and `Mcp-Param-*` routing headers are advisory and carry no
>   authority. See `docs/authenticated-principal-design.md`.
> - Authentication is repeated for every POST. Two requests on one kept-alive
>   connection are two independent authentications, so a revoked credential
>   stops working on the next request rather than on the next connection.
> - HTTP headers that mirror body fields are validated against the body and the
>   request is refused with `400` and `-32020` on any disagreement, after
>   decoding the Base64 sentinel form. This prevents a gateway routing on the
>   header while the runtime executes on the body.
> - Request line, header block, header count and body are independently
>   bounded, and the body bound is the same `max_message_bytes` the stdio
>   reader and the channel output budget derive from. Header reads, body reads
>   and JSON-mode writes each have their own deadline; an SSE stream is
>   deliberately unbounded in duration and bounded in queue depth by its
>   channel's outbox.
> - Any header value containing NUL, CR or LF is refused before it is
>   interpreted, and a repeated protocol header is refused rather than merged.
> - Request framing is unambiguous by refusal rather than by resolution. Any
>   `Transfer-Encoding` header is rejected outright, a `Transfer-Encoding`
>   together with a `Content-Length` is rejected and logged as a smuggling
>   attempt, a repeated or non-numeric `Content-Length` is rejected even when
>   the duplicates agree, and `obs-fold` continuations and bare-LF line endings
>   are rejected. Every framing rejection closes the connection; the parser
>   never resynchronizes to look for a following request.
> - Request pipelining is refused, and any inbound byte arriving between a
>   request and the completion of its response terminates the connection.
>   Connection reuse is permitted for `application/json` replies and refused
>   for event streams. Each request on a reused connection is authenticated
>   independently.
> - Closing the response stream is the cancellation signal. After a disconnect
>   the runtime admits no further frame for that request and writes no further
>   bytes; the provider's own call timeout bounds when its work actually stops.
>   A stream abandoned mid-body is closed without its terminal chunk, so a
>   truncated response is never framed as a complete one.
> - A request whose channel cannot be closed within its deadline releases its
>   connection and its network slot immediately; the channel is detached and
>   freed by its last in-flight operation. A stuck provider therefore cannot
>   exhaust the listener's connection capacity, and nothing is freed while a
>   worker can still reach it.
> - The protocol versions a channel serves and announces are fixed at channel
>   creation, so an HTTP channel cannot be talked into a legacy handshake and
>   `server/discover` over HTTP advertises only what HTTP serves. The transport
>   never rewrites a dispatch result to achieve this.
> - `Mcp-Session-Id` and `Last-Event-ID` are ignored and never echoed. GET and
>   DELETE on the MCP endpoint are refused with `405`, and any HTTP version
>   other than 1.1 with `505`.

And under **Trust boundary**, the paragraph at `docs/security-model.md:82`
("The runtime remains a local stdio host…") is replaced, because its first
clause stops being true in this release. Its *substance* does not change and
must be carried over: exposing the endpoint beyond loopback still requires an
authenticator and a per-principal effect policy, and providers still run with
host privileges and are not sandboxed. Its closing clause — that the middleware
chain's per-channel context is "the foundation a transport-established principal
will bind to" (`docs/security-model.md:85`) — stops being future tense and
becomes a description of what shipped.

## Phasing, and what each phase authorizes

Each phase has a merge criterion that is a fact about the tree rather than a
feeling about progress. No phase merges to `main` in a state where `make check`
is red.

The HTTP phases are **H1–H4**. The runtime work they depend on is **H0**,
numbered that way because it contains no HTTP code at all: it is runtime
surgery that happens to be a prerequisite, it improves stdio on its own merits,
and it is reviewable — and revertable — without reference to a transport.

### The authorization ladder

Phases are not authorized as a block. What each one may begin depends on what
has been accepted, and the conditions are different per phase:

| Phase | Authorized when |
|---|---|
| **H1** — server layer, adapter seam, parser | **These corrections landing.** No dependency on the principal seam or on H0: H1 touches no channel and dispatches nothing. |
| **H0a** — pollable outbox | Independently reviewable; may proceed in parallel with H1. |
| **H2** — MCP validation | **PR #63 accepted *and implemented*.** The ABI 4 config and `context_release` must exist before any phase binds a principal to a channel. *(As built the binding happened in H3, the first phase in which channels exist; H1 absorbed the authenticators. See [What shipped](#what-shipped-and-where).)* |
| **H3** — dispatch, JSON and SSE | PR #63 implemented **and** H0a landed. Without the first, principals leak per detached request; without the second, the cancellation chain does not exist. |
| **H4** — tests, TSan, conformance | H3. |

The conformance claim is unchanged and is not weakened by this ladder: the
official runner must pass against the **real binary**, with no adapter in the
path, before Streamable HTTP is claimed anywhere.

### H0 — runtime prerequisites

[H0a](#h0a--a-pollable-outbox) only: the lazy outbox wakeup descriptor,
`maelys_mcp_channel_enable_wait_fd` and `maelys_mcp_channel_wait_fd`, internal
to `src/internal/internal.h`.

H0b shipped in 0.17.0 and is no longer a phase. The
[protocol-era mask](#the-version-mask-and-what-it-fixes) and the context
destructor both belong to PR #63's ABI 4 change, not here.

*Merge criterion.* A test that enables the wait fd, polls it, and observes
readiness for every enqueue path — response, coalesced notification, and close
— and non-readiness on a drained outbox. A test that a channel that never
enabled it returns `-1` and allocates no pipe, which is the property that keeps
stdio's cost at zero. A test that the same-chunk and split-chunk enqueue
sequences produce the same readiness. ASan and TSan green; `tests/test_channel_perf.c`
figures do not regress for the stdio path.

### H1 — the server layer, the adapter seam, and the parser

Both sides of the new interface, together, because they are one contract:
`maelys_mcp_http_request_t`, `maelys_mcp_http_response_writer_t`,
`maelys_mcp_http_adapter_handle` and `_cancel` on the library side; and on the
host side the listener, the connection thread, the hand-rolled request-line and
header parser, every rule under
[Strict rejection rules](#strict-rejection-rules), the limits, `Host` and
`Origin` validation, `405`/`415`/`406`/`411`/`417`/`431`/`505`, the keep-alive
and pipelining policy, and the loopback default. The adapter answers from a
table: it validates nothing and dispatches nothing yet.

*Merge criterion.* Unit tests over every strict-rejection rule, including
split-across-chunks equivalence. `http-request`, `http-smuggling` and
`http-origin` in `fuzz-smoke`, with `http-smuggling`'s
single-unambiguous-length invariant asserted. A test that binding a
non-loopback address without an authenticator fails to start. A test that a
second request on a kept-alive connection is served and that a pipelined one is
refused. A recording writer drives the adapter in both JSON and stream modes
with no socket involved, and `scripts/audit_boundaries.sh` confirms
`libmaelys_mcp` gained no networking symbol.

### H2 — authentication and MCP validation

The authenticator seam from `docs/authenticated-principal-design.md`, with
`loopback-trust` and `static-bearer`; the authenticate-before-body ordering and
the bounded discard; `401`/`503`/`403` per that document's outcome table.
`MCP-Protocol-Version`, `Mcp-Method`, `Mcp-Name`, the Base64 sentinel, the
header↔body comparison, `-32020`, request/notification/response classification,
and the full status mapping table. Still no dispatch.

*Merge criterion.* A table-driven test over every row of the status mapping
table and of the outcome table. Sentinel round-trip tests including the
pattern-collision case and invalid UTF-8. A test that `authenticate` runs once
per POST and twice on a kept-alive connection carrying two. **A test that a
`401` is delivered and readable by the client while the client is still writing
a body** — the regression test for the reset hazard the bounded discard exists
to prevent. `http-headers` in `fuzz-smoke`.

### H3 — dispatch, JSON and SSE

`channel_create` with the principal and the era mask, `channel_accept`, the
`poll{socket, wait_fd}` drain loop, mode selection on the first frame, SSE
chunked framing and flushing, keep-alives, `202` for notifications, the full
cancellation chain, shutdown ordering, `subscriptions/listen` end to end.

*Merge criterion.* A `tools/call` with progress returns SSE with the frames in
order and the response last. A `subscriptions/listen` receives its
acknowledgement, then a fanned-out event, then `resultType: "complete"` on
shutdown. A client disconnect mid-call produces no further writes, and a
disconnect during a *wedged* provider call frees the connection slot within the
close deadline while the channel is freed later — the H0b end-to-end test. An
abandoned stream is closed **without** a terminal chunk. A `wrap_sink`
middleware registered over HTTP observes the same frame sequence it observes
over stdio — the test that proves ⑥ did not acquire a transport dependency.
`http-exchange` in `fuzz-smoke`.

### H4 — tests, TSan, and the bridge replacement

`make tsan` covers the HTTP transport. Perf baselines extend
`tests/test_channel_perf.c`. `conformance/run_official_mcp.py` loses
`StdioBridge` (`conformance/run_official_mcp.py:247`) and its `handler_for`
adapter (`:318`) for the modern pass, and points the official runner at the
real binary.

*Merge criterion.* Every scenario in `MODERN_SCENARIOS`
(`conformance/run_official_mcp.py:86`) passes against the real HTTP listener
with no adapter in the path, **plus** the two scenarios the bridge currently
excludes as transport-specific — `server-sse-multiple-streams` and
`dns-rebinding-protection` (`conformance/run_official_mcp.py:168`). Those two
are the acceptance test for this whole document: they are excluded today with
the reasons "this bridge does not implement SSE transport semantics" and "this
bridge does not implement Host/Origin header validation", and a design that
does not make both exclusions stale has not replaced the bridge, only moved it.

> **As built (#72), and the acceptance test passed.** Both scenarios the bridge
> could not run are now in `MODERN_SCENARIOS` and run **directly against the
> real HTTP listener** with no adapter in the path
> (`conformance/run_official_mcp.py:98`, `:106`) — `server-sse-multiple-streams`
> against the real chunked SSE framing, `dns-rebinding-protection` against the
> real `Host`/`Origin` validation. That was the stated acceptance test for this
> whole document, and it is the reason it was stated: a design that had not made
> both exclusions stale would have moved the bridge rather than replaced it.
>
> The modern pass runs the binary with `--http-listen`
> (`conformance/run_official_mcp.py:5`), 17 scenarios, one shared subprocess
> for the pass because 2026-07-28 is stateless and there is no session state to
> leak between scenarios.
>
> **The conformance story is now two transports, not one bridge.** The legacy
> pass keeps `StdioBridge` unchanged, because legacy stays stdio-only — so the
> two eras are exercised over the two transports that actually serve them,
> rather than both through one adapter that served neither. The exclusions for
> those two scenarios survive in `LEGACY_EXCLUDED` but are now annotated as
> legacy-only, pointing at `MODERN_SCENARIOS` for where they really run
> (`conformance/run_official_mcp.py:183`).
>
> What this still is not: a claim of complete Streamable HTTP conformance.
> `docs/protocol-support.md:67` names what remains outside the run.

## Not in v1

- **TLS.** The listener speaks cleartext HTTP/1.1 and is meant to sit behind a
  terminator, or on loopback. A TLS library is a large dependency whose payoff
  is the mTLS authenticator, which is itself out of scope — and an orchestrator
  that has already terminated TLS reuses the adapter directly
  ([layering](#layering-server-adapter-runtime)) rather than asking this
  listener to grow.
- **HTTP/2 and HTTP/3.** One protocol version, hand-rolled, is a reviewable
  parser. HPACK is not.
- **Chunked request bodies, and `Transfer-Encoding` in any form.** `411` when
  the length is simply missing, `400`-and-close when a `Transfer-Encoding`
  header is present at all. A `Content-Length` is what makes the body limit
  checkable before a byte is read; refusing the alternative outright is what
  removes the request-smuggling class rather than defending against it.
- **`Expect: 100-continue`.** `417`. Named here as well as in the parser rules
  because it is the one omission that makes another part of the design worse:
  it is the standard way to answer `401` before a body arrives.
- **`Last-Event-ID` resumption.** Removed by the revision.
- **Sessions, `Mcp-Session-Id`, GET SSE, DELETE.** Removed by the revision, and
  the whole reason this design is small.
- **`x-mcp-header` / `Mcp-Param-*` support.** Needs a schema keyword and a
  `tools/list` change; it is a tools feature.
- **Prompts.** No module (`conformance/run_official_mcp.py:120`), so
  `prompts/get` is `404`.
- **A connection thread pool.** See the threading section. The concurrency
  bound in v1 is the connection limit.
- **Compression.** No `Content-Encoding` in either direction. It interacts with
  the body limit in exactly the way that produces decompression bombs.
- **HTTP client leg.** Talking to an upstream MCP server over HTTP is the
  `mcp_proxy` provider's business and a separate milestone
  (`docs/middleware-design.md:556`).

## Open questions

**None remain open.** Every question earlier revisions raised has been decided
and, in each case, built. The ledger is kept because the decisions outlived the
questions and a reader arriving at a shipped transport is entitled to know which
of its properties were argued rather than assumed:

| Former question | Decision | Shipped as |
|---|---|---|
| `channel_destroy`'s unbounded wait | Detachable destruction | 0.17.0 (#55), `include/maelys/mcp/channel.h:188` |
| `server/discover` advertising a version HTTP does not serve | Per-channel era mask, as a config field | 0.18.0 (#64), `include/maelys/mcp/channel.h:81` |
| Endpoint path: transport's or host's? | The host's; `/mcp` is the standalone default | `include/maelys/mcp/http.h:25` |
| `401` vs `403` for a failed bearer | `401` + `WWW-Authenticate: Bearer`; `503` for "cannot decide" | #63, `host/http_auth.c` |
| ABI route for the era mask | Config field, ABI 3 → 4, setter removed | 0.18.0 (#64) |
| Is `channel_wait_fd` public? | Internal | `src/internal/internal.h:851` |
| Does ABI 4 ship before the transport or with it? | **Before.** ABI 4 landed in 0.18.0 (#64); the transport followed on top of it | H1–H4 |

That last one resolved itself in the direction this document hoped for without
recommending: the break was digested in its own release, so 0.19.0 carries a
transport and not a transport *plus* an ABI migration. The concern that argued
for deciding it explicitly — that `maelys_mcp_channel_set_protocol_eras` would
otherwise survive releases as a function the next release deletes — turned out
to be short-lived: it existed in 0.17.0 only.

One row of the [status table](#status-and-error-mapping) is deliberately
reserved with no producer, which is a documented gap rather than an open
question; see the note there.

## Where the code contradicted the drafts

Recorded because `docs/middleware-design.md` earned its authority by admitting
the same thing, and a design document that reports only its confirmed guesses
is not evidence of having read anything.

- **"A fourth fuzz target."** The brief this document was written from asked for
  a fourth libFuzzer target. There are already five (`Makefile:415`), and the
  HTTP boundary needs several rather than one. The ordinal is gone and the
  targets are enumerated by what they cover.
- **"The transport passes its own sink."** `src/internal/internal.h:731`
  explicitly offers that, and the first draft took it. It is wrong for
  `subscriptions/listen`, because fanout bypasses the sink entirely
  (`src/modules/subscriptions.c:397`). The design now uses the channel's own
  outbox sink and drains the queue, which is also what makes `wrap_sink`
  behave identically on both transports.
- **"`complete` ends the exchange."** True for every method except the one this
  transport most needs to get right: `listen` returns its acknowledgement as
  the dispatch response (`src/modules/subscriptions.c:303`), so `complete`
  carries a notification and the stream must stay open past it.
- **"Cancellation propagates to the provider."** It does not. `cancelled` is a
  poll (`src/core/channel.c:416`) and the actual bound is the provider's call
  timeout. The guarantee in the cancellation section was rewritten to claim
  only what the code delivers.
- **"`MCP-Protocol-Version` selects the era."** It does not and must not. The
  era is decided by `params._meta["io.modelcontextprotocol/protocolVersion"]`
  in the body (`src/core/runtime.c:674`); the header is *validated against*
  that value and is never substituted for it. Synthesizing `_meta` from a
  header would make a routing header authoritative over the payload, which is
  the inversion the spec's header/body rule exists to prevent.

Four more were found while revising, and the first is the most serious thing
either draft got wrong:

- **"The connection thread watches the socket while draining the outbox."**
  Not realizable against the API as it exists. `maelys_mcp_outbox_next` blocks
  on a condition variable (`src/core/outbox.c:371`), and a thread inside it is
  watching nothing — before the first frame, that wait lasts as long as the
  provider does. Keeping the worker offload was necessary but not sufficient;
  it takes the connection thread out of the *provider call* and leaves it
  blocked on a *condvar*. The whole cancellation chain rested on this and did
  not exist. [H0a](#h0a--a-pollable-outbox) is the fix, and it is a runtime
  change the draft did not know it needed.
- **"The project has a zero-dependency ethos."** It links jansson and
  uriparser (`Makefile:15`). The parser argument was restated as what it
  actually is — deliberately small and heavily fuzzed — which is a claim the
  fuzz targets can be held to.
- **"`runtime_destroy` will notice a detached channel."** It refuses only on
  `live_channel_count != 0` (`src/core/runtime.c:181`), and detaching
  *decrements* that counter. A reaper design that stopped at "the last worker
  frees it" would let a runtime be destroyed while a detached channel still
  held a pointer to it. `detached_channel_count` exists because of that check,
  not in spite of it.
- **"Per-channel configuration is free."** `docs/abi-policy.md:23` makes
  adding a field to `maelys_mcp_channel_config_t` an ABI break, and
  `src/internal/internal.h:193` records the project having previously chosen
  the other way for exactly this reason. The era mask is still the right
  design; its delivery is not free, and is now decided rather than discovered
  during implementation.
- **"An ABI 4 bump is the natural route."** Recommended, then withdrawn when
  `475333b` landed a paragraph naming additive entry points as the preferred
  idiom, then **reinstated by the owner** on the ground that there are no
  external users, so the migration costs a recompile and the cleanest API wins.
  Recorded as three moves rather than one because the middle one was correct
  on the evidence available and was overturned by a fact — zero users — that no
  amount of reading the tree would have supplied.

Four more were found while reconciling with the shipped 0.17.0:

- **"The last worker releases the principal."** Not implementable against the
  shipped API. `maelys_mcp_channel_destroy_detached` frees the channel later
  but there is no context-release callback for the last worker to invoke, and
  the header tells the embedder to wait for `maelys_mcp_runtime_destroy`
  instead (`include/maelys/mcp/channel.h:177`) — which for a long-lived server
  means principals accumulate for the whole process uptime. This is why PR #63
  is an API change and not a convention.
- **"The adapter never touches a socket."** Said in the same breath as handing
  it a `disconnect_fd` and having it call `recv(..., MSG_PEEK)`. Both could not
  be true. The cancellation source is now abstract and the FIN-versus-pipelined
  distinction stays in the server layer, which is the only layer that has the
  pipelining rule that makes the distinction meaningful.
- **"`end_stream` is called exactly once."** Stated alongside "an abandoned
  stream closes without the terminal chunk", which is incompatible if
  `end_stream` always writes one. It now takes a disposition, and both paths'
  exact bytes are specified.
- **"`maelys_mcp_http_headers_t` is opaque."** Unimplementable from outside the
  library: an external orchestrator could neither construct one nor read one,
  which defeats the entire point of splitting the adapter out. Replaced with a
  lookup callback the embedder supplies over whatever representation it already
  has.

And one about this document's own framing:

- **"This transport is the 0.17 flagship."** 0.17.0 shipped without it, and
  shipped several of its prerequisites instead. The transport landed on top of
  0.18.0 and is the 0.19.0 headline; the ledger at the top exists so no reader
  has to reconstruct which half of the original plan is already in the tree.

## Readiness

The three questions this section used to keep apart have all been answered by
building the thing, so what follows is the answer rather than the estimate.

**Design-only adversarial review: done, twice, and it paid.** Two rounds of
review against these documents caught, among others, a cancellation chain that
could not have been implemented — a thread blocked in
`maelys_mcp_outbox_next` is not watching a socket — an adapter API that no
external orchestrator could have implemented, and a `end_stream` contract that
contradicted itself. Each of those would have been found during implementation
instead, at a cost. The fourteen entries in
[Where the code contradicted the drafts](#where-the-code-contradicted-the-drafts)
are the record.

**Implementation: shipped.** H1 (#67), H2 (#69), H3 (#70), H4 (#72), on top of
the prerequisites in 0.17.0 (#55, #58, #59) and 0.18.0 (#63, #64). The binary
serves MCP over HTTP directly; `--http-listen` is a real transport and not a
test adapter.

**Streamable HTTP conformance: demonstrated for the run that is defined, and
still not claimed beyond it.** The official runner drives the real endpoint with
no bridge in the path, and the two scenarios that were the stated acceptance
test for this whole document — `server-sse-multiple-streams` and
`dns-rebinding-protection` — now pass against the real listener rather than
being excluded as impossible. What that is *not* is complete conformance:
`docs/protocol-support.md:67` names what stays outside the run, and it should
keep naming it.

This document therefore stops being a plan and becomes the design of record.
The convention `docs/middleware-design.md` set is the one to follow from here:
the design text stays as written, the **As built** notes carry what actually
happened, and where the two diverge the divergence is stated rather than
smoothed away — because a design document edited to agree with its outcome is
no longer evidence that the outcome was reasoned about.
