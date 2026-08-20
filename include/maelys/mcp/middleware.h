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
 * This release implements the design's two decision points - on_authorize
 * (hook 2) and on_audit (hook 5) - plus the registration, ordering and
 * invocation machinery the remaining hooks plug into.
 *
 * A chain is registered while the runtime is still cold and is immutable from
 * the first channel onward, so dispatch reads it without a lock. A runtime
 * with no middleware runs exactly the path it ran before the chain existed:
 * the per-hook counters are zero and no hook context is even built.
 */

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
 * Threading: both hooks run on the thread dispatching one request, serialized
 * within that request, and may block. They may be entered concurrently for
 * different channels, so a middleware sharing mutable state across channels
 * owns its own locking. No runtime lock is held while a hook runs.
 */
typedef struct maelys_mcp_middleware {
    const char *name;
    void *context;
    maelys_mcp_on_authorize_fn on_authorize;
    maelys_mcp_on_audit_fn on_audit;
    void (*destroy)(void *context);
} maelys_mcp_middleware_t;

/*
 * Appends one middleware to the chain. The descriptor is copied, so it may
 * live on the caller's stack; `name` and `context` are borrowed, not copied.
 *
 * Order is registration order, and it is the order hooks observe: on_authorize
 * runs front to back and stops at the first middleware that does not allow,
 * so an early deny is never overridden by a later allow; on_audit runs front
 * to back and every hook runs, whatever the outcome.
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
