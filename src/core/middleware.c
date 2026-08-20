#include "src/internal/internal.h"

#include <stdlib.h>

static void set_error(char **out_error, const char *message) {
    if (!out_error) return;
    free(*out_error);
    *out_error = maelys_mcp_strdup(message);
}

maelys_mcp_result_t maelys_mcp_runtime_add_middleware(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_middleware_t *middleware,
    char **out_error) {
    if (!runtime || !middleware) return MAELYS_MCP_ERR_ARGUMENT;
    /*
     * A middleware with no hook at all is a caller mistake, not an empty
     * chain: it would occupy a slot and observe nothing. Refusing it here
     * keeps "the chain is empty" and "the chain does nothing" the same
     * statement.
     */
    if (!middleware->on_authorize && !middleware->on_audit) {
        set_error(out_error, "middleware implements no hook");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    pthread_mutex_lock(&runtime->lifecycle_mutex);
    /*
     * Same gate as maelys_mcp_runtime_add_provider, for a stronger reason:
     * the hot path reads this array with no lock, so it must stop changing
     * before the first channel can dispatch through it.
     */
    if (runtime->lifecycle != MAELYS_MCP_RUNTIME_COLD ||
        runtime->shutdown_requested) {
        pthread_mutex_unlock(&runtime->lifecycle_mutex);
        return MAELYS_MCP_ERR_STATE;
    }
    if (runtime->middleware_count == MAELYS_MCP_MAX_MIDDLEWARE) {
        set_error(out_error, "middleware capacity reached");
        pthread_mutex_unlock(&runtime->lifecycle_mutex);
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    runtime->middleware[runtime->middleware_count++] = *middleware;
    if (middleware->on_authorize) runtime->authorize_hook_count++;
    if (middleware->on_audit) runtime->audit_hook_count++;
    pthread_mutex_unlock(&runtime->lifecycle_mutex);
    return MAELYS_MCP_OK;
}

int maelys_mcp_chain_has_authorize(const maelys_mcp_runtime_t *runtime) {
    return runtime && runtime->authorize_hook_count != 0u;
}

int maelys_mcp_chain_has_audit(const maelys_mcp_runtime_t *runtime) {
    return runtime && runtime->audit_hook_count != 0u;
}

maelys_mcp_authorize_decision_t maelys_mcp_chain_authorize(
    const maelys_mcp_runtime_t *runtime,
    const maelys_mcp_authorize_context_t *request) {
    if (!runtime || !request) return MAELYS_MCP_AUTHORIZE_ERROR;
    for (size_t index = 0; index < runtime->middleware_count; ++index) {
        const maelys_mcp_middleware_t *middleware = &runtime->middleware[index];
        if (!middleware->on_authorize) continue;
        maelys_mcp_authorize_decision_t decision =
            middleware->on_authorize(middleware->context, request);
        /*
         * First non-allow wins and the rest of the chain is not consulted: a
         * later middleware must never be able to overturn an earlier deny,
         * and a middleware that failed to reach a verdict has already made
         * the request unanswerable.
         */
        if (decision != MAELYS_MCP_AUTHORIZE_ALLOW) {
            return decision == MAELYS_MCP_AUTHORIZE_ERROR ?
                MAELYS_MCP_AUTHORIZE_ERROR : MAELYS_MCP_AUTHORIZE_DENY;
        }
    }
    return MAELYS_MCP_AUTHORIZE_ALLOW;
}

void maelys_mcp_chain_audit(
    const maelys_mcp_runtime_t *runtime,
    const maelys_mcp_audit_context_t *record) {
    if (!runtime || !record) return;
    /* Observational: every hook runs, none of them can change the outcome. */
    for (size_t index = 0; index < runtime->middleware_count; ++index) {
        const maelys_mcp_middleware_t *middleware = &runtime->middleware[index];
        if (middleware->on_audit) {
            middleware->on_audit(middleware->context, record);
        }
    }
}

void maelys_mcp_chain_destroy(maelys_mcp_runtime_t *runtime) {
    if (!runtime) return;
    /*
     * Reverse registration order, so a middleware registered later - and
     * therefore possibly built on an earlier one's state - is torn down
     * first.
     */
    size_t index = runtime->middleware_count;
    while (index != 0u) {
        const maelys_mcp_middleware_t *middleware = &runtime->middleware[--index];
        if (middleware->destroy) middleware->destroy(middleware->context);
    }
    runtime->middleware_count = 0u;
    runtime->authorize_hook_count = 0u;
    runtime->audit_hook_count = 0u;
}

/*
 * The compatibility middleware. It exists so that migrating an embedder off
 * the removed authorize/audit config fields is one call rather than a
 * rewrite, and it is deliberately the only thing in the runtime that still
 * knows the pre-chain metadata shape.
 */
typedef struct compat_policy {
    maelys_mcp_authorize_fn authorize;
    maelys_mcp_audit_fn audit;
    void *policy_context;
} compat_policy_t;

static maelys_mcp_authorize_decision_t compat_on_authorize(
    void *context,
    const maelys_mcp_authorize_context_t *request) {
    const compat_policy_t *policy = context;
    if (!policy->authorize) return MAELYS_MCP_AUTHORIZE_ALLOW;
    /*
     * The channel and the request params stop here: a callback written
     * against the old signature has no field to receive them in. That is the
     * cost of the compatibility path, and the reason to move to on_authorize.
     */
    maelys_mcp_request_context_t legacy = {
        .protocol_version = request->protocol_version,
        .client_name = request->client_name,
        .tool_name = request->tool_name,
        .resource_uri = request->resource_uri,
        .operation = request->operation,
        .effect = request->effect
    };
    /* Any non-zero return meant "allowed" before the chain existed. */
    return policy->authorize(policy->policy_context, &legacy) != 0 ?
        MAELYS_MCP_AUTHORIZE_ALLOW : MAELYS_MCP_AUTHORIZE_DENY;
}

static void compat_on_audit(
    void *context,
    const maelys_mcp_audit_context_t *record) {
    const compat_policy_t *policy = context;
    if (!policy->audit) return;
    /*
     * The old journal had one view, and it was the client's: pass the
     * requested identity, not the resolved one, so a future transform cannot
     * silently start writing injected values into an existing audit sink.
     */
    maelys_mcp_request_context_t legacy = {
        .protocol_version = record->protocol_version,
        .client_name = record->client_name,
        .tool_name = record->requested_tool_name,
        .resource_uri = record->requested_resource_uri,
        .operation = record->operation,
        .effect = record->effect
    };
    policy->audit(policy->policy_context, &legacy, record->outcome);
}

static void compat_destroy(void *context) {
    free(context);
}

maelys_mcp_result_t maelys_mcp_runtime_add_compat_policy(
    maelys_mcp_runtime_t *runtime,
    maelys_mcp_authorize_fn authorize,
    maelys_mcp_audit_fn audit,
    void *policy_context) {
    if (!runtime) return MAELYS_MCP_ERR_ARGUMENT;
    if (!authorize && !audit) return MAELYS_MCP_ERR_ARGUMENT;
    compat_policy_t *policy = calloc(1u, sizeof(*policy));
    if (!policy) return MAELYS_MCP_ERR_MEMORY;
    policy->authorize = authorize;
    policy->audit = audit;
    policy->policy_context = policy_context;
    maelys_mcp_middleware_t middleware = {
        .name = "compat-policy",
        .context = policy,
        .on_authorize = authorize ? compat_on_authorize : NULL,
        .on_audit = audit ? compat_on_audit : NULL,
        .destroy = compat_destroy
    };
    maelys_mcp_result_t status = maelys_mcp_runtime_add_middleware(
        runtime, &middleware, NULL);
    /* Registration never takes ownership on failure, so the shim state is
     * this function's to release. */
    if (status != MAELYS_MCP_OK) free(policy);
    return status;
}
