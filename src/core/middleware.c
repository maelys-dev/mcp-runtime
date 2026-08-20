#include "src/internal/internal.h"

#include <stdlib.h>
#include <string.h>

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
    if (!middleware->on_resolve && !middleware->on_authorize &&
        !middleware->on_call && !middleware->on_result &&
        !middleware->on_audit && !middleware->wrap_sink &&
        !middleware->on_list) {
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
    if (middleware->on_resolve) runtime->resolve_hook_count++;
    if (middleware->on_authorize) runtime->authorize_hook_count++;
    if (middleware->on_call) runtime->call_hook_count++;
    if (middleware->on_result) runtime->result_hook_count++;
    if (middleware->on_audit) runtime->audit_hook_count++;
    if (middleware->wrap_sink) runtime->wrap_sink_hook_count++;
    if (middleware->on_list) runtime->list_hook_count++;
    pthread_mutex_unlock(&runtime->lifecycle_mutex);
    return MAELYS_MCP_OK;
}

int maelys_mcp_chain_has_resolve(const maelys_mcp_runtime_t *runtime) {
    return runtime && runtime->resolve_hook_count != 0u;
}

int maelys_mcp_chain_has_authorize(const maelys_mcp_runtime_t *runtime) {
    return runtime && runtime->authorize_hook_count != 0u;
}

int maelys_mcp_chain_has_call(const maelys_mcp_runtime_t *runtime) {
    return runtime && runtime->call_hook_count != 0u;
}

int maelys_mcp_chain_has_result(const maelys_mcp_runtime_t *runtime) {
    return runtime && runtime->result_hook_count != 0u;
}

int maelys_mcp_chain_has_audit(const maelys_mcp_runtime_t *runtime) {
    return runtime && runtime->audit_hook_count != 0u;
}

int maelys_mcp_chain_has_wrap_sink(const maelys_mcp_runtime_t *runtime) {
    return runtime && runtime->wrap_sink_hook_count != 0u;
}

int maelys_mcp_chain_has_list(const maelys_mcp_runtime_t *runtime) {
    return runtime && runtime->list_hook_count != 0u;
}

/*
 * Hook 1. Every middleware runs and each sees the previous one's output, which
 * is what makes two independent transforms composable - a proxy prefix and a
 * per-principal injection, say - rather than mutually exclusive.
 */
maelys_mcp_result_t maelys_mcp_chain_resolve(
    const maelys_mcp_runtime_t *runtime,
    maelys_mcp_resolve_context_t *request,
    char **out_tool_name,
    json_t **out_arguments) {
    if (!runtime || !request || !out_tool_name || !out_arguments) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    *out_tool_name = NULL;
    *out_arguments = NULL;
    for (size_t index = 0; index < runtime->middleware_count; ++index) {
        const maelys_mcp_middleware_t *middleware = &runtime->middleware[index];
        if (!middleware->on_resolve) continue;
        maelys_mcp_resolution_t resolution = {0};
        maelys_mcp_result_t status =
            middleware->on_resolve(middleware->context, request, &resolution);
        if (status != MAELYS_MCP_OK) {
            free(resolution.tool_name);
            if (resolution.arguments) json_decref(resolution.arguments);
            goto failed;
        }
        if (resolution.tool_name) {
            free(*out_tool_name);
            *out_tool_name = resolution.tool_name;
            request->tool_name = *out_tool_name;
        }
        if (resolution.arguments) {
            /*
             * A non-object here would reach schema validation as something the
             * validator can only reject with a message about the client's
             * arguments, blaming the caller for the middleware's bug. Refuse it
             * where it happened instead.
             */
            if (!json_is_object(resolution.arguments)) {
                json_decref(resolution.arguments);
                goto failed;
            }
            if (*out_arguments) json_decref(*out_arguments);
            *out_arguments = resolution.arguments;
            request->arguments = *out_arguments;
        }
    }
    return MAELYS_MCP_OK;
failed:
    free(*out_tool_name);
    *out_tool_name = NULL;
    if (*out_arguments) json_decref(*out_arguments);
    *out_arguments = NULL;
    return MAELYS_MCP_ERR_STATE;
}

/*
 * Hook 3. First substitution wins and the rest of the chain is not consulted:
 * a cache in front of a proxy must not have the proxy run anyway.
 */
maelys_mcp_call_disposition_t maelys_mcp_chain_call(
    const maelys_mcp_runtime_t *runtime,
    const maelys_mcp_call_context_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    if (!runtime || !request || !out_result) return MAELYS_MCP_CALL_ERROR;
    for (size_t index = 0; index < runtime->middleware_count; ++index) {
        const maelys_mcp_middleware_t *middleware = &runtime->middleware[index];
        if (!middleware->on_call) continue;
        char *error = NULL;
        maelys_mcp_call_disposition_t disposition = middleware->on_call(
            middleware->context, request, out_result, &error);
        if (disposition == MAELYS_MCP_CALL_ERROR) {
            if (out_error && !*out_error) {
                *out_error = error;
            } else {
                free(error);
            }
            return MAELYS_MCP_CALL_ERROR;
        }
        if (disposition == MAELYS_MCP_CALL_SUBSTITUTE) {
            /* A substitution that wants to report a failure does it the way a
             * provider does, with is_error on the result it returns; any
             * message set alongside a successful substitution is noise. */
            free(error);
            return MAELYS_MCP_CALL_SUBSTITUTE;
        }
        /* INVOKE, or any value a future release adds: nothing was
         * substituted, so anything written is not an answer and is dropped. */
        free(error);
        maelys_mcp_provider_result_clear(out_result);
        maelys_mcp_provider_result_init(out_result);
    }
    return MAELYS_MCP_CALL_INVOKE;
}

/*
 * Hook 4. Every middleware runs and each sees the previous replacement, so a
 * truncator and a redactor compose in registration order.
 */
maelys_mcp_result_disposition_t maelys_mcp_chain_result(
    const maelys_mcp_runtime_t *runtime,
    maelys_mcp_result_context_t *request,
    maelys_mcp_provider_result_t *result,
    char **out_error) {
    if (!runtime || !request || !result) return MAELYS_MCP_RESULT_ERROR;
    maelys_mcp_result_disposition_t outcome = MAELYS_MCP_RESULT_UNCHANGED;
    for (size_t index = 0; index < runtime->middleware_count; ++index) {
        const maelys_mcp_middleware_t *middleware = &runtime->middleware[index];
        if (!middleware->on_result) continue;
        maelys_mcp_provider_result_t replacement;
        maelys_mcp_provider_result_init(&replacement);
        char *error = NULL;
        request->result = result;
        maelys_mcp_result_disposition_t disposition = middleware->on_result(
            middleware->context, request, &replacement, &error);
        if (disposition == MAELYS_MCP_RESULT_REPLACED) {
            free(error);
            maelys_mcp_provider_result_clear(result);
            *result = replacement;
            outcome = MAELYS_MCP_RESULT_REPLACED;
            continue;
        }
        maelys_mcp_provider_result_clear(&replacement);
        if (disposition == MAELYS_MCP_RESULT_ERROR) {
            if (out_error && !*out_error) {
                *out_error = error;
            } else {
                free(error);
            }
            return MAELYS_MCP_RESULT_ERROR;
        }
        free(error);
    }
    request->result = result;
    return outcome;
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

/*
 * Hook 7. Chained like hook 1: every middleware runs and each sees the
 * previous catalog, so a filter and a renamer compose.
 */
maelys_mcp_result_t maelys_mcp_chain_list(
    const maelys_mcp_runtime_t *runtime,
    maelys_mcp_list_context_t *request,
    json_t **out_entries) {
    if (!runtime || !request || !out_entries) return MAELYS_MCP_ERR_ARGUMENT;
    *out_entries = NULL;
    for (size_t index = 0; index < runtime->middleware_count; ++index) {
        const maelys_mcp_middleware_t *middleware = &runtime->middleware[index];
        if (!middleware->on_list) continue;
        json_t *entries = NULL;
        maelys_mcp_result_t status =
            middleware->on_list(middleware->context, request, &entries);
        if (status != MAELYS_MCP_OK || (entries && !json_is_array(entries))) {
            if (entries) json_decref(entries);
            if (*out_entries) json_decref(*out_entries);
            *out_entries = NULL;
            return MAELYS_MCP_ERR_STATE;
        }
        if (!entries) continue;
        if (*out_entries) json_decref(*out_entries);
        *out_entries = entries;
        request->entries = entries;
    }
    return MAELYS_MCP_OK;
}

/*
 * Hook 6. The sink a middleware is handed is one of these: a fixed forwarder
 * whose context is the wrapper the middleware filled, so an unset function
 * pointer costs a branch and forwards untouched rather than obliging every
 * wrapper to reimplement pass-through three times.
 */
maelys_mcp_result_t maelys_mcp_sink_emit(
    const maelys_mcp_response_sink_t *sink,
    json_t *message) {
    if (!sink || !sink->emit || !message) return MAELYS_MCP_ERR_ARGUMENT;
    return sink->emit(sink->context, message);
}

maelys_mcp_result_t maelys_mcp_sink_complete(
    const maelys_mcp_response_sink_t *sink,
    json_t *response) {
    if (!sink || !sink->complete || !response) return MAELYS_MCP_ERR_ARGUMENT;
    return sink->complete(sink->context, response);
}

int maelys_mcp_sink_cancelled(const maelys_mcp_response_sink_t *sink) {
    /* No sink is a closed one: a caller with nowhere to write is cancelled. */
    if (!sink || !sink->cancelled) return 1;
    return sink->cancelled(sink->context);
}

static maelys_mcp_result_t link_emit(void *context, json_t *message) {
    const maelys_mcp_sink_link_t *link = context;
    if (!link->wrapper->emit) return maelys_mcp_sink_emit(link->inner, message);
    return link->wrapper->emit(link->wrapper->context, link->inner, message);
}

static maelys_mcp_result_t link_complete(void *context, json_t *response) {
    const maelys_mcp_sink_link_t *link = context;
    if (!link->wrapper->complete) {
        return maelys_mcp_sink_complete(link->inner, response);
    }
    return link->wrapper->complete(link->wrapper->context, link->inner, response);
}

static int link_cancelled(void *context) {
    const maelys_mcp_sink_link_t *link = context;
    if (!link->wrapper->cancelled) return maelys_mcp_sink_cancelled(link->inner);
    return link->wrapper->cancelled(link->wrapper->context, link->inner);
}

static maelys_mcp_result_t guard_emit(void *context, json_t *message) {
    maelys_mcp_sink_chain_t *chain = context;
    return maelys_mcp_sink_emit(chain->base, message);
}

static maelys_mcp_result_t guard_complete(void *context, json_t *response) {
    maelys_mcp_sink_chain_t *chain = context;
    /*
     * Exactly once, enforced. Two responses carrying one request id is a
     * protocol violation whichever of them a client believes, so the second is
     * refused here rather than delivered - and refused with the sink's own
     * failure convention, leaving the message with its caller.
     */
    if (chain->completions != 0u) return MAELYS_MCP_ERR_STATE;
    chain->completions++;
    return maelys_mcp_sink_complete(chain->base, response);
}

static int guard_cancelled(void *context) {
    maelys_mcp_sink_chain_t *chain = context;
    return maelys_mcp_sink_cancelled(chain->base);
}

maelys_mcp_result_t maelys_mcp_chain_wrap_sink(
    const maelys_mcp_runtime_t *runtime,
    const maelys_mcp_wrap_sink_context_t *request,
    const maelys_mcp_response_sink_t *base,
    maelys_mcp_sink_chain_t *chain,
    const maelys_mcp_response_sink_t **out_sink) {
    if (!runtime || !request || !base || !chain || !out_sink) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    chain->depth = 0u;
    chain->completions = 0u;
    chain->runtime = runtime;
    chain->base = base;
    chain->guard.emit = guard_emit;
    chain->guard.complete = guard_complete;
    chain->guard.cancelled = guard_cancelled;
    chain->guard.context = chain;
    const maelys_mcp_response_sink_t *inner = &chain->guard;
    for (size_t index = 0; index < runtime->middleware_count; ++index) {
        const maelys_mcp_middleware_t *middleware = &runtime->middleware[index];
        if (!middleware->wrap_sink) continue;
        maelys_mcp_sink_wrapper_t *wrapper = &chain->wrappers[chain->depth];
        memset(wrapper, 0, sizeof(*wrapper));
        maelys_mcp_result_t status =
            middleware->wrap_sink(middleware->context, request, wrapper);
        if (status != MAELYS_MCP_OK) {
            /* This wrapper never took ownership of anything - a wrap_sink that
             * fails must clean up after itself - but the ones already built
             * did, and they are released in reverse. */
            maelys_mcp_chain_release_sink(chain);
            return status;
        }
        chain->owners[chain->depth] = index;
        maelys_mcp_response_sink_t *sink = &chain->sinks[chain->depth];
        /*
         * The link's context is the slot itself, which is why sinks[] and
         * wrappers[] are parallel arrays in the caller's frame rather than
         * anything allocated: the address a sink hands back has to stay valid
         * for the whole dispatch, and a stack frame that outlives the request
         * by construction is the cheapest guarantee of that.
         */
        chain->links[chain->depth].wrapper = wrapper;
        chain->links[chain->depth].inner = inner;
        sink->emit = link_emit;
        sink->complete = link_complete;
        sink->cancelled = link_cancelled;
        sink->context = &chain->links[chain->depth];
        inner = sink;
        chain->depth++;
    }
    *out_sink = chain->depth != 0u ? inner : base;
    return MAELYS_MCP_OK;
}

void maelys_mcp_chain_release_sink(maelys_mcp_sink_chain_t *chain) {
    if (!chain) return;
    /* Reverse wrapping order, so the outermost wrapper - the one that may have
     * been built on an inner one's state - is torn down first. */
    while (chain->depth != 0u) {
        maelys_mcp_sink_wrapper_t *wrapper = &chain->wrappers[--chain->depth];
        if (wrapper->release) wrapper->release(wrapper->context);
    }
}

size_t maelys_mcp_chain_sink_completions(const maelys_mcp_sink_chain_t *chain) {
    return chain ? chain->completions : 0u;
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
    runtime->resolve_hook_count = 0u;
    runtime->authorize_hook_count = 0u;
    runtime->call_hook_count = 0u;
    runtime->result_hook_count = 0u;
    runtime->audit_hook_count = 0u;
    runtime->wrap_sink_hook_count = 0u;
    runtime->list_hook_count = 0u;
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
