#pragma once

#include <jansson.h>

#include "maelys/mcp/channel.h"
#include "maelys/mcp/error.h"
#include "maelys/mcp/provider.h"
#include "maelys/mcp/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The middleware chain is the runtime's only policy and observation seam. It
 * replaces the runtime-global authorize/audit callback pair that
 * maelys_mcp_runtime_config_t carried through ABI 2; the two never coexist,
 * because two similar-looking mechanisms on the security-critical path double
 * the surface that has to be audited, and because a callback placed before
 * name resolution and one placed after it mean different things. See
 * docs/middleware.md for the surface and docs/middleware-design.md for why it
 * has this shape.
 *
 * All seven hooks of the design are implemented: the two decision points -
 * on_authorize (hook 2) and on_audit (hook 5) - and the five transformation
 * hooks - on_resolve (1), on_call (3), on_result (4), wrap_sink (6) and
 * on_list (7).
 *
 * A chain is registered while the runtime is still cold and is immutable from
 * the first channel onward, so dispatch reads it without a lock. A runtime
 * with no middleware runs exactly the path it ran before the chain existed:
 * the per-hook counters are zero and no hook context is even built.
 */

/*
 * ---- Hook 1: on_resolve ----------------------------------------------------
 *
 * The request-side transformation point, and the only one. It turns what the
 * client wrote - a published name, a published argument set - into what the
 * runtime will actually run. Everything downstream sees the resolved form:
 * hook 2 decides on it, schema validation checks it, the provider receives it.
 *
 * Two consequences are worth stating before the fields, because both are
 * places a transform can go quietly wrong.
 *
 * Arguments must be valid against the REAL tool's schema. Validation runs
 * after this hook and checks the underlying tool, never the schema hook 7
 * published. A transform that changes an argument's type must therefore
 * convert it here; rewriting the type in hook 7 alone makes validation reject
 * every call.
 *
 * The audit keeps the client's view, not this one. Hook 5 records what the
 * request carried, so a value injected here - the canonical example is an API
 * key hidden from the published schema - never reaches a journal through hook
 * 5. That is deliberate, and it is why maelys_mcp_audit_context_t has no
 * resolved-arguments field.
 *
 * Every hook 1 runs, front to back, and each sees the previous one's output.
 * A hook that returns anything other than MAELYS_MCP_OK fails the request with
 * -32603 and the provider is never reached.
 */
typedef struct maelys_mcp_resolve_context {
    /* MAELYS_MCP_OPERATION_CALL today; resources/read does not resolve yet. */
    maelys_mcp_operation_t operation;
    /*
     * The name to resolve: what the client wrote, or what an earlier hook 1
     * already rewrote it to. Borrowed for the call.
     */
    const char *tool_name;
    /*
     * The call's arguments, never NULL - a call that carried none arrives as
     * an empty object, so a hook injecting a value need not special-case it.
     * Borrowed and read-only; return a replacement rather than mutating it.
     */
    json_t *arguments;
    /*
     * The request's whole params, borrowed and read-only, on the same terms as
     * hook 2's: pre-validation, therefore untrusted, and carrying the MRTR
     * continuation siblings inputResponses and requestState.
     */
    json_t *params;
    const char *protocol_version;
    /* Client-asserted; see maelys_mcp_authorize_context_t.client_name. */
    const char *client_name;
    const maelys_mcp_channel_t *channel;
} maelys_mcp_resolve_context_t;

/*
 * What hook 1 changes. Leaving a field NULL means "unchanged", and an
 * all-NULL resolution is the zero-copy path: nothing is allocated and nothing
 * is copied.
 *
 * Both non-NULL fields transfer ownership to the runtime, which releases
 * tool_name with free() and arguments with json_decref(). tool_name must
 * therefore come from malloc/strdup, never from a string literal or a
 * middleware-owned table.
 */
typedef struct maelys_mcp_resolution {
    char *tool_name;
    /* Must be a JSON object; anything else fails the request. */
    json_t *arguments;
} maelys_mcp_resolution_t;

typedef maelys_mcp_result_t (*maelys_mcp_on_resolve_fn)(
    void *context,
    const maelys_mcp_resolve_context_t *request,
    maelys_mcp_resolution_t *out_resolution);

/*
 * Hook 2's verdict. Only ALLOW lets the operation proceed: every other value,
 * including one a future release adds and this one does not know, denies.
 * That polarity is deliberate - a middleware returning a zeroed decision
 * fails closed - and it preserves the legacy convention where a non-zero
 * authorize() return meant "allowed".
 *
 * The three outcomes map to three distinct results, never to one:
 *   ALLOW - the operation proceeds;
 *   DENY  - JSON-RPC -32003 (MCP policy denied) for a call or a read; the
 *           entry is silently omitted from a list;
 *   ERROR - JSON-RPC -32603 (internal error). The middleware could not reach
 *           a verdict; that is a fault in the policy adapter, not a decision
 *           about the caller, and it must not be reported as a denial.
 */
typedef enum maelys_mcp_authorize_decision {
    MAELYS_MCP_AUTHORIZE_DENY = 0,
    MAELYS_MCP_AUTHORIZE_ALLOW = 1,
    MAELYS_MCP_AUTHORIZE_ERROR = 2
} maelys_mcp_authorize_decision_t;

/*
 * What hook 2 decides on. Every pointer is borrowed for the duration of the
 * call and must not be retained.
 */
typedef struct maelys_mcp_authorize_context {
    maelys_mcp_operation_t operation;
    /*
     * The tool's declared effect for a tool operation, and
     * MAELYS_MCP_EFFECT_READ for every resource operation - reading is the
     * only effect the resource surface has.
     */
    maelys_mcp_tool_effect_t effect;
    /*
     * The resolved identity, exactly one of the two being set: tool_name for
     * MAELYS_MCP_OPERATION_LIST and _CALL, resource_uri for the three
     * resource operations. The resource URI is the canonical form, after
     * parsing and normalization, never the string the client sent.
     *
     * "Resolved" is the invariant, not "registry-backed": once hook 1 exists,
     * a rename must be resolved before this hook sees it, so that
     * transformation can never route around a decision, and a synthetic tool
     * with no registry entry still arrives here.
     */
    const char *tool_name;
    const char *resource_uri;
    const char *protocol_version;
    /*
     * Client-asserted, unauthenticated, and defaulted to "unknown" in both
     * protocol eras. Fine to record; never base a decision on it. The channel
     * context below is the only anchor worth authenticating against.
     */
    const char *client_name;
    /*
     * The channel the request arrived on. Pass it to
     * maelys_mcp_channel_context() for the pointer the embedder bound at
     * channel creation - the per-principal, per-session anchor without which
     * no chain can tell two clients of one runtime apart.
     */
    const maelys_mcp_channel_t *channel;
    /*
     * The request's whole params object, or NULL when it carried none.
     *
     * This is what gives hook 2 sight of MRTR continuation traffic:
     * inputResponses (elicitation answers and sampling completions - the most
     * sensitive inbound payloads in the protocol) and the opaque requestState
     * are siblings of arguments, so a hook seeing only a name and its
     * arguments would be blind to every continuation. It does not make a
     * continuation channel-bound: a continuation may legitimately arrive on a
     * different channel than the one that issued it, and this release does
     * not change that.
     *
     * Two properties matter to anyone reading it. It is pre-validation - hook
     * 2 deliberately runs before schema validation, so a denied caller cannot
     * probe argument schemas through validation error details - which makes
     * these params untrusted input, checked only for the envelope fields
     * dispatch itself needed. And it is read-only: mutating it from a hook is
     * undefined. Rewriting arguments is hook 1's exclusive job.
     */
    json_t *params;
} maelys_mcp_authorize_context_t;

/*
 * What hook 5 records. Observational and fire-and-forget: its return is void,
 * it cannot change an outcome, and every registered on_audit runs whatever
 * the outcome was.
 *
 * It carries both views on purpose. The journal needs what the client asked
 * for and what actually executed, because the difference between them is
 * where intent lives - and because an audit seeing only the post-resolution
 * state would log values a transform injected, which is exactly how a hidden
 * credential ends up in a log file. The requested_* fields are therefore the
 * client-facing view and the ones to record; the resolved fields say what
 * ran. Until hook 1 exists the two are always equal, and code written against
 * the pair keeps working when they diverge.
 */
typedef struct maelys_mcp_audit_context {
    maelys_mcp_operation_t operation;
    maelys_mcp_tool_effect_t effect;
    /* Client-facing view: the name or URI as the request carried it. */
    const char *requested_tool_name;
    const char *requested_resource_uri;
    /* Resolved view: what the runtime actually executed. */
    const char *tool_name;
    const char *resource_uri;
    const char *protocol_version;
    /* Client-asserted; see maelys_mcp_authorize_context_t.client_name. */
    const char *client_name;
    const maelys_mcp_channel_t *channel;
    /* The client's params, borrowed and read-only; NULL when there were none. */
    json_t *params;
    /*
     * MAELYS_MCP_OK when the operation completed, MAELYS_MCP_ERR_DENIED when
     * hook 2 denied it, and whatever the provider or result validation
     * produced otherwise.
     */
    maelys_mcp_result_t outcome;
} maelys_mcp_audit_context_t;

typedef maelys_mcp_authorize_decision_t (*maelys_mcp_on_authorize_fn)(
    void *context,
    const maelys_mcp_authorize_context_t *request);

typedef void (*maelys_mcp_on_audit_fn)(
    void *context,
    const maelys_mcp_audit_context_t *record);

/*
 * ---- Hook 3: on_call -------------------------------------------------------
 *
 * Invoke or substitute, and nothing else. A hook 3 either lets the call reach
 * the provider or answers it itself; it cannot rewrite the arguments on the
 * way, which is why they arrive read-only.
 *
 * That constraint is the whole point of having flat hooks rather than one
 * onion wrapper. Arguments mutated inside a wrapper would be mutated after
 * schema validation, which defeats the ordering the chain exists to pin. The
 * price is real and worth naming: FastMCP's forward() pattern - "validate,
 * maybe raise, then call the original with remapped arguments" - splits in two
 * here, hook 1 for the remapping and hook 3 for the short circuit.
 *
 * Hooks run front to back and stop at the first SUBSTITUTE; a cache or a proxy
 * ahead of a slower link is the shape this ordering is for.
 */
typedef enum maelys_mcp_call_disposition {
    /* Nothing was substituted; the call continues. The zero, so a middleware
     * returning a zeroed disposition never injects an empty result. */
    MAELYS_MCP_CALL_INVOKE = 0,
    /* out_result carries the answer; the provider is not reached. */
    MAELYS_MCP_CALL_SUBSTITUTE = 1,
    /* The hook failed: -32603, and out_error becomes the message. */
    MAELYS_MCP_CALL_ERROR = 2
} maelys_mcp_call_disposition_t;

typedef struct maelys_mcp_call_context {
    /* The resolved tool - what hook 2 allowed and what the provider will be
     * asked for - and, for the record, what the client actually wrote. */
    const char *tool_name;
    const char *requested_tool_name;
    maelys_mcp_tool_effect_t effect;
    /*
     * The resolved arguments, already validated against the real tool's input
     * schema. Read-only: mutating them from here is undefined, and rewriting
     * them is hook 1's exclusive job.
     */
    json_t *arguments;
    /* The request's whole params, borrowed and read-only. */
    json_t *params;
    const char *protocol_version;
    const char *client_name;
    const maelys_mcp_channel_t *channel;
} maelys_mcp_call_context_t;

/*
 * A substituting hook fills out_result exactly as a provider's call() fills
 * its own: one initialized maelys_mcp_provider_result_t whose JSON fields are
 * newly owned references, released by the runtime. The substituted result is
 * validated like a provider's, output schema included - hook 3 stands in for
 * the provider, so it answers to the same contract.
 */
typedef maelys_mcp_call_disposition_t (*maelys_mcp_on_call_fn)(
    void *context,
    const maelys_mcp_call_context_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error);

/*
 * ---- Hook 4: on_result -----------------------------------------------------
 *
 * The result-side transformation point: redaction, truncation, output
 * rewriting. Every hook 4 runs, front to back, and each sees the previous
 * one's replacement.
 *
 * Where it sits relative to validation is a decision, not an accident. The
 * runtime validates the REAL result against the REAL schema before this hook
 * runs, so a provider that breaks its own contract is caught whatever the
 * chain does. A replacement is then re-checked structurally - it must still be
 * a result the runtime can serialize, and its content blocks must still pass
 * the wire checks - but NOT against the tool's outputSchema, because producing
 * a value the published schema no longer describes is precisely what redaction
 * is. Keeping hook 7's advertised outputSchema consistent with what hook 4
 * emits is the middleware author's responsibility; the runtime cannot check it
 * and does not pretend to.
 *
 * This hook runs only on a successful, validated result. A failed provider
 * call has no result object to rewrite, and its error text reaches the client
 * unchanged.
 */
typedef enum maelys_mcp_result_disposition {
    /* Zero copy: nothing was written and the result stands. */
    MAELYS_MCP_RESULT_UNCHANGED = 0,
    /* out_result replaces it; the runtime releases the old one. */
    MAELYS_MCP_RESULT_REPLACED = 1,
    /* The hook failed: -32603, and out_error becomes the message. */
    MAELYS_MCP_RESULT_ERROR = 2
} maelys_mcp_result_disposition_t;

typedef struct maelys_mcp_result_context {
    const char *tool_name;
    const char *requested_tool_name;
    maelys_mcp_tool_effect_t effect;
    /* The resolved arguments, borrowed and read-only. */
    json_t *arguments;
    json_t *params;
    const char *protocol_version;
    const char *client_name;
    const maelys_mcp_channel_t *channel;
    /*
     * What the provider - or a substituting hook 3 - produced, and what an
     * earlier hook 4 may already have replaced it with. Borrowed and
     * read-only; a rewrite is returned, never applied in place.
     */
    const maelys_mcp_provider_result_t *result;
    /*
     * The real tool's declared output schema, or NULL when it declares none.
     * Borrowed. It is here so a hook that must stay schema-valid can see what
     * it has to satisfy - the runtime does not re-check a replacement against
     * it.
     */
    json_t *output_schema;
} maelys_mcp_result_context_t;

typedef maelys_mcp_result_disposition_t (*maelys_mcp_on_result_fn)(
    void *context,
    const maelys_mcp_result_context_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error);

/*
 * ---- Hook 6: wrap_sink -----------------------------------------------------
 *
 * The response sink is where one request's outbound frames leave the runtime:
 * every request-scoped notification it emits - notifications/progress today -
 * and then its single final response. Hook 6 decorates that path, which makes
 * it the seam for progress coalescing and for redacting a stream rather than a
 * buffered result.
 *
 * The sink itself stays opaque. A wrapper never builds one; it forwards to the
 * inner sink it was handed, through the three functions below.
 */
typedef struct maelys_mcp_response_sink maelys_mcp_response_sink_t;

/*
 * Forwarding, on the outbox's ownership convention: emit and complete steal
 * the caller's reference on success and leave it with the caller on failure,
 * so a wrapper that gets a non-OK result still owns its message.
 */
maelys_mcp_result_t maelys_mcp_sink_emit(
    const maelys_mcp_response_sink_t *sink,
    json_t *message);
maelys_mcp_result_t maelys_mcp_sink_complete(
    const maelys_mcp_response_sink_t *sink,
    json_t *response);
/* Non-zero once this request's channel can no longer take output. */
int maelys_mcp_sink_cancelled(const maelys_mcp_response_sink_t *sink);

/*
 * One request's decoration of the sink. Every function is optional: a NULL
 * member forwards to the inner sink untouched, so "coalesce progress and leave
 * everything else alone" is one function pointer.
 *
 * `context` is this wrapper's own per-request state, distinct from the
 * middleware's context - allocate it in wrap_sink and free it in `release`,
 * which the runtime calls once when the request is finished, whatever the
 * outcome, and after which no function here is entered again.
 *
 * Two obligations the runtime enforces rather than trusts:
 *   - `complete` reaches the inner sink exactly once. A wrapper that swallows
 *     it would wedge the request forever, so the runtime detects that and
 *     answers the client with -32603 rather than leaving it waiting; a second
 *     forwarded completion is refused with MAELYS_MCP_ERR_STATE, because two
 *     responses for one id is a protocol violation.
 *   - ordering. Request-scoped notifications are delivered strictly ahead of
 *     the final response, and the runtime never calls `complete` before
 *     dispatch has returned - so a wrapper preserves that ordering simply by
 *     forwarding in the order it is called. A wrapper that BUFFERS emitted
 *     frames must flush them before forwarding `complete`; over SSE the final
 *     response terminates the stream and anything flushed after it is lost.
 *
 * Threading: all three run on the thread dispatching the request, serialized
 * within it, with no runtime lock held - the same contract as hooks 1 to 5.
 * Wrappers for different channels may run concurrently.
 */
typedef struct maelys_mcp_sink_wrapper {
    void *context;
    maelys_mcp_result_t (*emit)(
        void *context,
        const maelys_mcp_response_sink_t *inner,
        json_t *message);
    maelys_mcp_result_t (*complete)(
        void *context,
        const maelys_mcp_response_sink_t *inner,
        json_t *response);
    int (*cancelled)(
        void *context,
        const maelys_mcp_response_sink_t *inner);
    void (*release)(void *context);
} maelys_mcp_sink_wrapper_t;

typedef struct maelys_mcp_wrap_sink_context {
    const maelys_mcp_channel_t *channel;
    /*
     * The whole JSON-RPC request, borrowed and read-only. The sink is wrapped
     * before dispatch parses anything, so this is raw: not yet checked for a
     * method, an id, or a protocol era. Treat it as untrusted input.
     */
    json_t *request;
} maelys_mcp_wrap_sink_context_t;

/*
 * Returns MAELYS_MCP_OK having filled out_wrapper, or any other result to fail
 * the request with -32603. Wrapping order is the reverse of hook 4's: the
 * first-registered middleware is the closest to the client, so it is the last
 * to see an outbound frame before it reaches the transport.
 */
typedef maelys_mcp_result_t (*maelys_mcp_wrap_sink_fn)(
    void *context,
    const maelys_mcp_wrap_sink_context_t *request,
    maelys_mcp_sink_wrapper_t *out_wrapper);

/*
 * ---- Hook 7: on_list -------------------------------------------------------
 *
 * Catalog transformation: the other half of hook 1. Renaming a tool, hiding an
 * argument from its published schema, filtering a catalog per principal and
 * adding synthetic entries all happen here - and all of them, except pure
 * filtering, need a matching hook 1 to resolve calls back to something real.
 *
 * It runs after hook 2 has filtered the catalog, so a denied entry is already
 * gone and cannot be transformed back into view. An entry this hook invents
 * has no registry backing, which is allowed and is what a proxy or a
 * retrieval-first meta-tool needs; a call to it still passes hook 1 and hook
 * 2, so a synthetic name is not a way around a decision.
 *
 * Every hook 7 runs, front to back, each seeing the previous one's output.
 */
typedef enum maelys_mcp_catalog_kind {
    MAELYS_MCP_CATALOG_TOOLS = 0,
    MAELYS_MCP_CATALOG_RESOURCES = 1,
    MAELYS_MCP_CATALOG_RESOURCE_TEMPLATES = 2
} maelys_mcp_catalog_kind_t;

typedef struct maelys_mcp_list_context {
    maelys_mcp_catalog_kind_t catalog;
    /*
     * The entries that survived hook 2, as a JSON array in the shape they
     * would be serialized in - tool objects for MAELYS_MCP_CATALOG_TOOLS,
     * resource objects, resource-template objects. Borrowed and read-only.
     */
    json_t *entries;
    json_t *params;
    const char *protocol_version;
    const char *client_name;
    const maelys_mcp_channel_t *channel;
} maelys_mcp_list_context_t;

/*
 * Returns MAELYS_MCP_OK, leaving *out_entries NULL for "unchanged" - the zero
 * copy path - or setting it to a newly owned JSON array the runtime takes
 * over. Anything that is not an array, and any non-OK result, fails the
 * listing with -32603: a catalog that could not be transformed is not a
 * shorter catalog, for the same reason an undecidable hook 2 is not a denial.
 */
typedef maelys_mcp_result_t (*maelys_mcp_on_list_fn)(
    void *context,
    const maelys_mcp_list_context_t *request,
    json_t **out_entries);

/*
 * One link in the chain. A middleware implements only the hooks it needs; a
 * NULL hook is skipped and costs nothing, so "just an authorizer" is one
 * field.
 *
 * `name` is borrowed and must outlive the runtime - it exists for diagnostics
 * and for reading a registration list, not for lookup. `context` is opaque to
 * the runtime, which never dereferences it. `destroy`, if set, is called once
 * with that context from maelys_mcp_runtime_destroy, in reverse registration
 * order, and is the only cleanup the runtime performs on a middleware's
 * behalf.
 *
 * Threading: every hook runs on the thread dispatching one request,
 * serialized within that request, and may block. They may be entered
 * concurrently for different channels, so a middleware sharing mutable state
 * across channels owns its own locking. No runtime lock is held while a hook
 * runs.
 *
 * The fields are in hook order, which is also the order one request meets
 * them: resolve, authorize, call, result, audit - with wrap_sink established
 * around the whole of it and on_list standing in for the four of them that a
 * listing never reaches.
 */
typedef struct maelys_mcp_middleware {
    const char *name;
    void *context;
    maelys_mcp_on_resolve_fn on_resolve;
    maelys_mcp_on_authorize_fn on_authorize;
    maelys_mcp_on_call_fn on_call;
    maelys_mcp_on_result_fn on_result;
    maelys_mcp_on_audit_fn on_audit;
    maelys_mcp_wrap_sink_fn wrap_sink;
    maelys_mcp_on_list_fn on_list;
    void (*destroy)(void *context);
} maelys_mcp_middleware_t;

/*
 * Appends one middleware to the chain. The descriptor is copied, so it may
 * live on the caller's stack; `name` and `context` are borrowed, not copied.
 *
 * Order is registration order, and it is the order hooks observe.
 * `on_authorize` runs front to back and stops at the first middleware that
 * does not allow, so an early deny is never overridden by a later allow;
 * `on_call` runs front to back and stops at the first substitution.
 * `on_resolve`, `on_result`, `on_list` and `on_audit` run front to back and
 * every hook runs - the first three chained, each seeing the previous one's
 * output, and `on_audit` whatever the outcome was. `wrap_sink` is the one
 * reversal: the first-registered middleware wraps innermost, so it is the
 * last to touch an outbound frame before the transport does.
 *
 * Registration is only legal while the runtime is cold - before the first
 * channel, exactly like maelys_mcp_runtime_add_provider - and returns
 * MAELYS_MCP_ERR_STATE afterwards; that immutability is what lets dispatch
 * read the chain with no lock. Ownership of `context` never transfers: on
 * failure nothing is registered and `destroy` is not called.
 */
maelys_mcp_result_t maelys_mcp_runtime_add_middleware(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_middleware_t *middleware,
    char **out_error);

/*
 * The metadata the pre-chain authorize/audit callbacks received. Kept for the
 * compatibility middleware below and for nothing else; new code decides on
 * maelys_mcp_authorize_context_t, which additionally carries the channel and
 * the request params.
 */
typedef struct maelys_mcp_request_context {
    const char *protocol_version;
    const char *client_name;
    const char *tool_name;
    const char *resource_uri;
    maelys_mcp_operation_t operation;
    maelys_mcp_tool_effect_t effect;
} maelys_mcp_request_context_t;

typedef int (*maelys_mcp_authorize_fn)(
    void *context,
    const maelys_mcp_request_context_t *request);

typedef void (*maelys_mcp_audit_fn)(
    void *context,
    const maelys_mcp_request_context_t *request,
    maelys_mcp_result_t outcome);

/*
 * Registers the built-in compatibility middleware: a chain link implementing
 * hooks 2 and 5 that calls a pre-chain authorize/audit pair with exactly the
 * metadata it used to receive. An embedder that set three fields on
 * maelys_mcp_runtime_config_t migrates by making one call with the same three
 * values, before its first channel.
 *
 * It reproduces the old decision surface, not the old observable ordering:
 * policy now runs ahead of schema validation, which is a deliberate, named
 * change (a denied caller must not be able to probe argument schemas through
 * validation error details), and a denied resource read is now audited like a
 * denied tool call always was. Either callback may be NULL. `policy_context`
 * is borrowed and must outlive the runtime; the shim's own small state is
 * allocated here and released by maelys_mcp_runtime_destroy.
 */
maelys_mcp_result_t maelys_mcp_runtime_add_compat_policy(
    maelys_mcp_runtime_t *runtime,
    maelys_mcp_authorize_fn authorize,
    maelys_mcp_audit_fn audit,
    void *policy_context);

#ifdef __cplusplus
}
#endif
