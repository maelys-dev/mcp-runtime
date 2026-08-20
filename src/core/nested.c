/*
 * Nested (in-band) multi-round-trip support: the host opens a real
 * server-to-client request in the middle of a call and blocks for its reply.
 *
 * This is MCP's older MRTR pattern, the one a schema-validating `2025-11-25`
 * client understands, and the counterpart of the resumable `input_required`
 * result the runtime already speaks. Both reference SDKs implement it with an
 * id-keyed pending-request table on a single-threaded event loop; with no async
 * runtime to lean on, the same shape here is an id-keyed table plus the worker
 * thread that carries one call, guarded by the channel mutex the rest of the
 * channel's per-connection state already uses.
 *
 * Nothing in this file knows what a transport is. That is the point: a future
 * HTTP transport reuses the table and the demux verbatim, and stdio.c stays a
 * thin adapter over both.
 */
#include "src/internal/internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

maelys_mcp_result_t maelys_mcp_channel_nested_init(
    maelys_mcp_channel_t *channel) {
    size_t capacity = channel->runtime->nested.max_nested_requests;
    if (!capacity) capacity = MAELYS_MCP_DEFAULT_MAX_NESTED_REQUESTS;
    channel->nested_requests = calloc(capacity, sizeof(*channel->nested_requests));
    if (!channel->nested_requests) return MAELYS_MCP_ERR_MEMORY;
    channel->nested_capacity = capacity;
    channel->nested_timeout_ms = channel->runtime->nested.request_timeout_ms ?
        channel->runtime->nested.request_timeout_ms :
        MAELYS_MCP_DEFAULT_NESTED_REQUEST_TIMEOUT_MS;
    return MAELYS_MCP_OK;
}

static void release_entry_locked(maelys_mcp_nested_request_t *entry) {
    if (entry->outer_id) json_decref(entry->outer_id);
    if (entry->payload) json_decref(entry->payload);
    memset(entry, 0, sizeof(*entry));
}

void maelys_mcp_channel_nested_clear(maelys_mcp_channel_t *channel) {
    if (!channel || !channel->nested_requests) return;
    for (size_t index = 0; index < channel->nested_capacity; ++index) {
        release_entry_locked(&channel->nested_requests[index]);
    }
    free(channel->nested_requests);
    channel->nested_requests = NULL;
    channel->nested_capacity = 0u;
}

/*
 * Settling never frees the entry: the thread waiting on it owns that, and
 * takes the payload with it. Freeing here would hand a woken waiter a slot
 * that has already been reused.
 */
static void settle_entry_locked(
    maelys_mcp_nested_request_t *entry,
    maelys_mcp_result_t status,
    json_t *payload) {
    if (!entry->in_use || entry->settled) {
        if (payload) json_decref(payload);
        return;
    }
    entry->settled = 1;
    entry->status = status;
    if (entry->payload) json_decref(entry->payload);
    entry->payload = payload;
}

void maelys_mcp_channel_nested_fail_all_locked(
    maelys_mcp_channel_t *channel,
    maelys_mcp_result_t status) {
    if (!channel || !channel->nested_requests) return;
    int woke = 0;
    for (size_t index = 0; index < channel->nested_capacity; ++index) {
        maelys_mcp_nested_request_t *entry = &channel->nested_requests[index];
        if (!entry->in_use || entry->settled) continue;
        settle_entry_locked(entry, status, NULL);
        woke = 1;
    }
    if (woke && channel->nested_ready_initialized) {
        pthread_cond_broadcast(&channel->nested_ready);
    }
}

void maelys_mcp_channel_nested_fail_all(
    maelys_mcp_channel_t *channel,
    maelys_mcp_result_t status) {
    if (!channel) return;
    pthread_mutex_lock(&channel->mutex);
    maelys_mcp_channel_nested_fail_all_locked(channel, status);
    pthread_mutex_unlock(&channel->mutex);
}

void maelys_mcp_channel_nested_fail_id(
    maelys_mcp_channel_t *channel,
    const char *nested_id,
    maelys_mcp_result_t status) {
    if (!channel || !nested_id || !*nested_id) return;
    pthread_mutex_lock(&channel->mutex);
    for (size_t index = 0; index < channel->nested_capacity; ++index) {
        maelys_mcp_nested_request_t *entry = &channel->nested_requests[index];
        if (!entry->in_use || entry->settled) continue;
        if (strcmp(entry->id, nested_id) != 0) continue;
        settle_entry_locked(entry, status, NULL);
        pthread_cond_broadcast(&channel->nested_ready);
        break;
    }
    pthread_mutex_unlock(&channel->mutex);
}

/*
 * `notifications/cancelled` names the outer call, not the nested request the
 * provider happens to have open underneath it - the client has no reason to
 * know that id exists. Indexing entries by their outer call's id too is what
 * lets a cancel reach through.
 */
void maelys_mcp_channel_nested_cancel_outer(
    maelys_mcp_channel_t *channel,
    const json_t *outer_id) {
    if (!channel || !outer_id) return;
    pthread_mutex_lock(&channel->mutex);
    int woke = 0;
    for (size_t index = 0; index < channel->nested_capacity; ++index) {
        maelys_mcp_nested_request_t *entry = &channel->nested_requests[index];
        if (!entry->in_use || entry->settled || !entry->outer_id) continue;
        if (!json_equal(entry->outer_id, outer_id)) continue;
        settle_entry_locked(entry, MAELYS_MCP_ERR_CLOSED, NULL);
        woke = 1;
    }
    if (woke) pthread_cond_broadcast(&channel->nested_ready);
    pthread_mutex_unlock(&channel->mutex);
}

int maelys_mcp_channel_nested_resolve(
    maelys_mcp_channel_t *channel,
    json_t *message) {
    if (!channel || !json_is_object(message)) return 0;
    json_t *id = json_object_get(message, "id");
    if (!json_is_string(id) || maelys_mcp_json_string_has_nul(id)) return 0;
    json_t *result = json_object_get(message, "result");
    json_t *error = json_object_get(message, "error");
    /* Exactly one of the two, and no `method`: anything else is a request or a
     * malformed frame, and belongs to the dispatcher. */
    if (json_object_get(message, "method") || (!!result == !!error)) return 0;
    const char *value = json_string_value(id);
    int matched = 0;
    pthread_mutex_lock(&channel->mutex);
    for (size_t index = 0; index < channel->nested_capacity; ++index) {
        maelys_mcp_nested_request_t *entry = &channel->nested_requests[index];
        if (!entry->in_use || entry->settled) continue;
        if (strcmp(entry->id, value) != 0) continue;
        settle_entry_locked(entry,
            result ? MAELYS_MCP_OK : MAELYS_MCP_ERR_PROVIDER,
            json_incref(result ? result : error));
        pthread_cond_broadcast(&channel->nested_ready);
        matched = 1;
        break;
    }
    pthread_mutex_unlock(&channel->mutex);
    return matched;
}

/*
 * The three client surfaces MCP defines for a server-initiated request, and
 * the capability each one requires. Identical to the set validate_input_requests
 * enforces for the resumable pattern, on purpose: which surfaces a provider may
 * reach must not depend on which MRTR shape it chose.
 */
static const char *required_capability(const char *method) {
    if (strcmp(method, "elicitation/create") == 0) return "elicitation";
    if (strcmp(method, "sampling/createMessage") == 0) return "sampling";
    if (strcmp(method, "roots/list") == 0) return "roots";
    return NULL;
}

static void set_error(char **out_error, const char *message) {
    if (!out_error) return;
    free(*out_error);
    *out_error = maelys_mcp_strdup(message);
}

/* Claims a slot and names it. Caller holds channel->mutex. */
static maelys_mcp_nested_request_t *claim_entry_locked(
    maelys_mcp_channel_t *channel,
    json_t *outer_id) {
    for (size_t index = 0; index < channel->nested_capacity; ++index) {
        maelys_mcp_nested_request_t *entry = &channel->nested_requests[index];
        if (entry->in_use) continue;
        memset(entry, 0, sizeof(*entry));
        entry->in_use = 1;
        (void)snprintf(entry->id, sizeof(entry->id), "%s%llu",
            MAELYS_MCP_NESTED_ID_PREFIX, ++channel->next_nested_id);
        entry->outer_id = outer_id ? json_deep_copy(outer_id) : NULL;
        return entry;
    }
    return NULL;
}

static json_t *nested_request_message(
    const char *id,
    const char *method,
    json_t *params) {
    json_t *message = json_object();
    /*
     * A copy of the params, not a reference to them. This frame goes to the
     * outbox and is released by whichever thread drains it, while `params`
     * belongs to the caller and is released on this one; sharing a node
     * between the two is two threads releasing one jansson refcount. A call
     * that carried no params still sends an empty object, so a client never
     * has to special-case a missing member.
     */
    json_t *body = params ? json_deep_copy(params) : json_object();
    if (!message || !body ||
        json_object_set_new(message, "jsonrpc", json_string("2.0")) != 0 ||
        json_object_set_new(message, "id", json_string(id)) != 0 ||
        json_object_set_new(message, "method", json_string(method)) != 0 ||
        json_object_set(message, "params", body) != 0) {
        if (message) json_decref(message);
        if (body) json_decref(body);
        return NULL;
    }
    json_decref(body);
    return message;
}

maelys_mcp_result_t maelys_mcp_provider_request_client(
    maelys_mcp_nested_relay_t *relay,
    const char *method,
    json_t *params,
    json_t **out_result,
    char **out_error) {
    if (!out_result) return MAELYS_MCP_ERR_ARGUMENT;
    *out_result = NULL;
    if (!method || !*method) return MAELYS_MCP_ERR_ARGUMENT;
    if (!relay || !relay->channel || !relay->sink || !relay->sink->emit) {
        set_error(out_error, "this call cannot open a nested client request");
        return MAELYS_MCP_ERR_STATE;
    }
    if (params && !json_is_object(params)) {
        set_error(out_error, "nested request params must be an object");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    const char *capability = required_capability(method);
    if (!capability) {
        set_error(out_error, "unsupported nested request method");
        return MAELYS_MCP_ERR_DENIED;
    }
    /*
     * Checked here, before anything is sent, and against what the client
     * actually declared - a provider must not be able to reach a client
     * surface that was never offered just by choosing the nested shape over
     * the resumable one.
     */
    if (!json_is_object(relay->client_capabilities) ||
        !json_is_object(json_object_get(relay->client_capabilities, capability))) {
        set_error(out_error, "client did not declare a required capability");
        return MAELYS_MCP_ERR_DENIED;
    }
    maelys_mcp_channel_t *channel = relay->channel;

    /*
     * Registered before it is sent, never after: a client fast enough to
     * answer while this thread was still between the two would otherwise have
     * its reply seen as a stray response and dispatched as a protocol error.
     */
    pthread_mutex_lock(&channel->mutex);
    if (channel->state != MAELYS_MCP_CHANNEL_ACTIVE) {
        pthread_mutex_unlock(&channel->mutex);
        set_error(out_error, "channel is no longer active");
        return MAELYS_MCP_ERR_CLOSED;
    }
    maelys_mcp_nested_request_t *entry = claim_entry_locked(channel,
        relay->outer_id);
    if (!entry) {
        pthread_mutex_unlock(&channel->mutex);
        set_error(out_error, "nested request capacity reached");
        return MAELYS_MCP_ERR_STATE;
    }
    char nested_id[MAELYS_MCP_NESTED_ID_MAX];
    (void)snprintf(nested_id, sizeof(nested_id), "%s", entry->id);
    unsigned int timeout_ms = channel->nested_timeout_ms;
    pthread_mutex_unlock(&channel->mutex);

    maelys_mcp_result_t status = MAELYS_MCP_OK;
    json_t *message = nested_request_message(nested_id, method, params);
    if (!message) {
        status = MAELYS_MCP_ERR_MEMORY;
    } else {
        /*
         * Through this request's own sink rather than straight into the
         * outbox: it is a frame of this request, so it must stay ordered ahead
         * of the final response and stay visible to a wrap_sink middleware
         * instead of going around it.
         */
        status = relay->sink->emit(relay->sink->context, message);
        if (status != MAELYS_MCP_OK) json_decref(message);
    }
    if (status != MAELYS_MCP_OK) {
        pthread_mutex_lock(&channel->mutex);
        release_entry_locked(entry);
        pthread_mutex_unlock(&channel->mutex);
        set_error(out_error, "cannot deliver the nested request");
        return status;
    }

    uint64_t deadline_ms = 0u;
    int have_deadline = maelys_mcp_monotonic_deadline(
        timeout_ms, &deadline_ms) == 0;
    /*
     * Published only for the window this thread is actually blocked, so a
     * provider that dies mid-wait can cancel it instead of leaving this thread
     * holding an operation reference until the nested deadline.
     */
    if (relay->waiter_bind) {
        relay->waiter_bind(relay->waiter_context, channel, nested_id);
    }
    pthread_mutex_lock(&channel->mutex);
    while (!entry->settled) {
        if (!have_deadline) {
            settle_entry_locked(entry, MAELYS_MCP_ERR_IO, NULL);
            break;
        }
        int waited = maelys_mcp_cond_wait_until(&channel->nested_ready,
            &channel->mutex, deadline_ms);
        if (waited == ETIMEDOUT) {
            settle_entry_locked(entry, MAELYS_MCP_ERR_TIMEOUT, NULL);
            break;
        }
        if (waited != 0) {
            settle_entry_locked(entry, MAELYS_MCP_ERR_IO, NULL);
            break;
        }
    }
    status = entry->status;
    json_t *payload = entry->payload;
    entry->payload = NULL;
    release_entry_locked(entry);
    pthread_mutex_unlock(&channel->mutex);
    if (relay->waiter_bind) {
        relay->waiter_bind(relay->waiter_context, NULL, NULL);
    }

    if (status == MAELYS_MCP_OK || status == MAELYS_MCP_ERR_PROVIDER) {
        *out_result = payload;
        if (status == MAELYS_MCP_ERR_PROVIDER) {
            json_t *client_message = json_is_object(payload) ?
                json_object_get(payload, "message") : NULL;
            set_error(out_error, json_is_string(client_message) &&
                !maelys_mcp_json_string_has_nul(client_message) ?
                json_string_value(client_message) : "client refused the request");
        }
        return status;
    }
    if (payload) json_decref(payload);
    set_error(out_error,
        status == MAELYS_MCP_ERR_TIMEOUT ? "nested request deadline exceeded" :
        (status == MAELYS_MCP_ERR_CLOSED ?
            "nested request was cancelled" : "nested request failed"));
    return status;
}
