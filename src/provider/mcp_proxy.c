#include "src/internal/internal.h"
#include "maelys/mcp/version.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/*
 * A provider that is an MCP *client*.
 *
 * It spawns a third-party MCP server over stdio, negotiates an era with it,
 * snapshots its catalog once, and re-exposes that snapshot through
 * maelys_mcp_provider_create - so effect gating, schema validation, policy and
 * audit apply to federated traffic exactly as they do to a native provider.
 * Reusing the ordinary provider path is the whole point: nothing downstream
 * knows the tools came from somewhere else.
 *
 * The threading shape is process_provider.c's, deliberately: one dedicated
 * reader thread, single-outstanding calls under exchange_mutex, and progress
 * frames queued by the reader and drained by the thread inside the call, never
 * emitted from the reader - the sink lives on the calling thread's stack frame
 * and is only that thread's to use.
 */

#define PROXY_MAX_PENDING_PROGRESS 256
#define PROXY_SHUTDOWN_TIMEOUT_MS MAELYS_MCP_DEFAULT_PROVIDER_SHUTDOWN_TIMEOUT_MS
#define PROXY_WRITE_TIMEOUT_MS MAELYS_MCP_DEFAULT_STDIO_WRITE_TIMEOUT_MS

typedef enum proxy_era {
    PROXY_ERA_MODERN = 0,
    PROXY_ERA_LEGACY = 1
} proxy_era_t;

/*
 * The pinned identity from docs/middleware-design.md: an upstream's tool names
 * are remote-controlled and may change between list and call, so calls resolve
 * against the snapshot taken at connect and never against a fresh list.
 */
typedef struct proxy_binding {
    char *exposed_name;
    char *upstream_name;
    /* Index of this tool's description inside the tools/list snapshot's
     * "tools" array - no longer the same as this binding's own index once
     * MAELYS_MCP_PROXY_SCHEMA_SKIP can drop entries out of the middle. */
    size_t source_index;
    /* Set when schema_policy is MAELYS_MCP_PROXY_SCHEMA_PASSTHROUGH and this
     * tool's own inputSchema failed validation: the catalog exposes the
     * permissive {"type":"object"} schema for it instead. */
    int passthrough;
} proxy_binding_t;

typedef struct proxy_context {
    /*
     * The upstream, seen only through the seam - no pid, for the same reason
     * the native provider has none: EOF on fd is the runtime's sole liveness
     * signal, and an upstream started inside a container or by an executord
     * must fit here unchanged.
     */
    maelys_mcp_process_slot_t slot;
    int fd;
    size_t max_message_bytes;
    unsigned int call_timeout_ms;
    maelys_mcp_line_reader_t *reader;
    pthread_mutex_t exchange_mutex;
    pthread_mutex_t write_mutex;
    pthread_mutex_t state_mutex;
    pthread_cond_t response_ready;
    int exchange_mutex_initialized;
    int write_mutex_initialized;
    int state_mutex_initialized;
    int response_ready_initialized;
    pthread_t reader_thread;
    int reader_started;
    int closing;
    int failed;
    int waiting;
    maelys_mcp_result_t failure_status;
    char *failure_message;
    unsigned long long next_id;
    unsigned long long expected_id;
    json_t *pending_response;
    /*
     * Progress frames the reader has taken off the wire but not yet delivered.
     * Same contract as maelys_mcp_process_context_t.pending_progress: the
     * caller drains this while it waits, so every emission happens on the
     * thread that owns the sink and passes through sink->emit in order, ahead
     * of the response.
     */
    json_t *pending_progress;
    /* The token this proxy generated for the call in flight, or NULL when the
     * client asked for no progress. Guarded by state_mutex. */
    json_t *progress_token;
    /*
     * Written once during connect, before the context is published through
     * maelys_mcp_provider_create, and read-only afterwards - the same
     * discipline the process provider applies to its parsed catalog.
     */
    proxy_era_t era;
    proxy_binding_t *bindings;
    size_t binding_count;
} proxy_context_t;

static void replace_error(char **out_error, const char *message) {
    if (!out_error) return;
    free(*out_error);
    *out_error = maelys_mcp_strdup(message ? message : "upstream MCP server error");
}

static void set_proxy_failure_locked(
    proxy_context_t *proxy,
    maelys_mcp_result_t status,
    const char *message) {
    if (proxy->failed) return;
    proxy->failed = 1;
    proxy->failure_status = status;
    proxy->failure_message = maelys_mcp_strdup(
        message ? message : "upstream MCP transport failed");
    pthread_cond_broadcast(&proxy->response_ready);
}

/* What is left of the one connect budget, so spawn, negotiation and the single
 * tools/list share it rather than each getting a full one. Zero means spent. */
static unsigned int remaining_ms(uint64_t deadline_ms) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0u;
    uint64_t current = (uint64_t)now.tv_sec * 1000u +
        (uint64_t)now.tv_nsec / 1000000u;
    if (current >= deadline_ms) return 0u;
    uint64_t left = deadline_ms - current;
    return left > (uint64_t)UINT_MAX ? UINT_MAX : (unsigned int)left;
}

/* Called only from the thread inside the provider call, never from the
 * reader: `reporter` points into that thread's stack frame. */
static void emit_pending_progress(
    maelys_mcp_progress_reporter_t *reporter,
    json_t *frames) {
    size_t index;
    json_t *frame;
    json_array_foreach(frames, index, frame) {
        json_t *progress = json_object_get(frame, "progress");
        json_t *total = json_object_get(frame, "total");
        json_t *message = json_object_get(frame, "message");
        if (!json_is_number(progress)) continue;
        (void)maelys_mcp_provider_report_progress(reporter,
            json_number_value(progress),
            json_is_number(total) ? json_number_value(total) : -1.0,
            json_is_string(message) && !maelys_mcp_json_string_has_nul(message) ?
                json_string_value(message) : NULL);
    }
}

static maelys_mcp_result_t proxy_write(proxy_context_t *proxy, json_t *message) {
    pthread_mutex_lock(&proxy->write_mutex);
    maelys_mcp_result_t status = maelys_mcp_write_json_line_with_timeout(
        proxy->fd, message, PROXY_WRITE_TIMEOUT_MS);
    pthread_mutex_unlock(&proxy->write_mutex);
    return status;
}

/*
 * A legacy upstream may open a real server-to-client request (sampling,
 * elicitation). v1 does not relay nested requests, so the request is refused
 * and the transport stays usable - refusing is the one answer that neither
 * strands the upstream nor invents a downstream client to ask.
 */
static void proxy_refuse_upstream_request(proxy_context_t *proxy, json_t *id) {
    json_t *response = maelys_mcp_error_response(id, JSONRPC_METHOD_NOT_FOUND,
        "Method not found", NULL);
    if (!response) return;
    (void)proxy_write(proxy, response);
    json_decref(response);
}

static void queue_progress_frame(proxy_context_t *proxy, json_t *message) {
    json_t *params = json_object_get(message, "params");
    json_t *token = json_is_object(params) ?
        json_object_get(params, "progressToken") : NULL;
    pthread_mutex_lock(&proxy->state_mutex);
    if (proxy->waiting && proxy->progress_token && token &&
        json_equal(token, proxy->progress_token)) {
        if (!proxy->pending_progress) proxy->pending_progress = json_array();
        if (proxy->pending_progress) {
            while (json_array_size(proxy->pending_progress) >=
                PROXY_MAX_PENDING_PROGRESS) {
                (void)json_array_remove(proxy->pending_progress, 0);
            }
            (void)json_array_append(proxy->pending_progress, params);
            pthread_cond_broadcast(&proxy->response_ready);
        }
    }
    pthread_mutex_unlock(&proxy->state_mutex);
}

static void *proxy_reader_main(void *opaque) {
    proxy_context_t *proxy = opaque;
    for (;;) {
        json_t *message = NULL;
        char *error = NULL;
        maelys_mcp_result_t status = maelys_mcp_line_reader_read(
            proxy->reader, proxy->fd, &message, &error);
        if (status != MAELYS_MCP_OK) {
            pthread_mutex_lock(&proxy->state_mutex);
            if (!proxy->closing) {
                set_proxy_failure_locked(proxy,
                    status == MAELYS_MCP_ERR_NOT_FOUND ?
                        MAELYS_MCP_ERR_PROVIDER : status,
                    error ? error : "upstream MCP server closed the transport");
            }
            pthread_mutex_unlock(&proxy->state_mutex);
            free(error);
            break;
        }
        if (!json_is_object(message)) {
            json_decref(message);
            pthread_mutex_lock(&proxy->state_mutex);
            set_proxy_failure_locked(proxy, MAELYS_MCP_ERR_PROTOCOL,
                "upstream MCP server sent a non-object JSON-RPC message");
            pthread_mutex_unlock(&proxy->state_mutex);
            break;
        }
        json_t *id = json_object_get(message, "id");
        json_t *method = json_object_get(message, "method");
        int identified = id && !json_is_null(id);
        if (json_is_string(method)) {
            if (identified) {
                proxy_refuse_upstream_request(proxy, id);
                json_decref(message);
                continue;
            }
            /*
             * Tolerance, and a deliberate difference from the strictness the
             * maelys-provider wire enforces: we do not control upstreams, and
             * an unknown notification (logging, resource updates, whatever a
             * server chooses to send) must not fault a transport that is
             * otherwise working. Only progress for the call in flight is
             * acted on; everything else is dropped.
             */
            if (maelys_mcp_json_string_equals(method, "notifications/progress")) {
                queue_progress_frame(proxy, message);
            }
            json_decref(message);
            continue;
        }
        if (!identified) {
            json_decref(message);
            pthread_mutex_lock(&proxy->state_mutex);
            set_proxy_failure_locked(proxy, MAELYS_MCP_ERR_PROTOCOL,
                "upstream MCP server sent a message with neither method nor id");
            pthread_mutex_unlock(&proxy->state_mutex);
            break;
        }
        /* An id-bearing response is correlated traffic, so an unexpected one
         * is a genuine protocol failure rather than noise to ignore. */
        pthread_mutex_lock(&proxy->state_mutex);
        if (!proxy->waiting || proxy->pending_response || !json_is_integer(id) ||
            (unsigned long long)json_integer_value(id) != proxy->expected_id) {
            set_proxy_failure_locked(proxy, MAELYS_MCP_ERR_PROTOCOL,
                "upstream MCP server returned an unexpected response id");
            pthread_mutex_unlock(&proxy->state_mutex);
            json_decref(message);
            break;
        }
        proxy->pending_response = message;
        pthread_cond_broadcast(&proxy->response_ready);
        pthread_mutex_unlock(&proxy->state_mutex);
    }
    return NULL;
}

static json_t *proxy_build_request(
    unsigned long long id,
    const char *method,
    json_t *params) {
    json_t *request = json_object();
    if (!request) return NULL;
    if (json_object_set_new(request, "jsonrpc", json_string("2.0")) != 0 ||
        json_object_set_new(request, "id", json_integer((json_int_t)id)) != 0 ||
        json_object_set_new(request, "method", json_string(method)) != 0 ||
        json_object_set(request, "params", params) != 0) {
        json_decref(request);
        return NULL;
    }
    return request;
}

static int attach_progress_token(json_t *params, json_t *token) {
    json_t *meta = json_object_get(params, "_meta");
    if (!json_is_object(meta)) {
        meta = json_object();
        if (!meta || json_object_set_new(params, "_meta", meta) != 0) {
            if (meta) json_decref(meta);
            return -1;
        }
    }
    /* progressToken is a bare key in both eras - the modern _meta namespaces
     * its own fields but not this one. */
    return json_object_set(meta, "progressToken", token);
}

/*
 * One request, one response, strictly single-outstanding per upstream.
 * `params` is stolen: the exchange owns it and may add the progress token to
 * it before the request goes out.
 */
static maelys_mcp_result_t proxy_exchange(
    proxy_context_t *proxy,
    const char *method,
    json_t *params,
    unsigned int timeout_ms,
    int want_progress,
    maelys_mcp_progress_reporter_t *reporter,
    json_t **out_result,
    char **out_error) {
    if (!proxy || proxy->fd < 0 || !method || !params || !out_result) {
        if (params) json_decref(params);
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    *out_result = NULL;
    uint64_t deadline_ms = 0u;
    if (!timeout_ms) {
        json_decref(params);
        replace_error(out_error, "no time left in the upstream MCP connect budget");
        return MAELYS_MCP_ERR_IO;
    }
    if (maelys_mcp_monotonic_deadline(timeout_ms, &deadline_ms) != 0) {
        json_decref(params);
        replace_error(out_error, "cannot compute the upstream MCP deadline");
        return MAELYS_MCP_ERR_IO;
    }
    pthread_mutex_lock(&proxy->exchange_mutex);
    pthread_mutex_lock(&proxy->state_mutex);
    if (proxy->failed || proxy->closing) {
        replace_error(out_error, proxy->failure_message ?
            proxy->failure_message : "upstream MCP transport is unavailable");
        maelys_mcp_result_t failed = proxy->failure_status ?
            proxy->failure_status : MAELYS_MCP_ERR_PROVIDER;
        pthread_mutex_unlock(&proxy->state_mutex);
        pthread_mutex_unlock(&proxy->exchange_mutex);
        json_decref(params);
        return failed;
    }
    unsigned long long id = ++proxy->next_id;
    proxy->expected_id = id;
    proxy->waiting = 1;
    json_t *token = NULL;
    if (want_progress) {
        char rendered[64];
        (void)snprintf(rendered, sizeof(rendered), "maelys-proxy/%llu", id);
        token = json_string(rendered);
        proxy->progress_token = token ? json_incref(token) : NULL;
    }
    pthread_mutex_unlock(&proxy->state_mutex);

    json_t *request = NULL;
    if (want_progress && (!token || attach_progress_token(params, token) != 0)) {
        request = NULL;
    } else {
        request = proxy_build_request(id, method, params);
    }
    json_decref(params);
    if (token) json_decref(token);
    if (!request) {
        pthread_mutex_lock(&proxy->state_mutex);
        proxy->waiting = 0;
        if (proxy->progress_token) json_decref(proxy->progress_token);
        proxy->progress_token = NULL;
        pthread_mutex_unlock(&proxy->state_mutex);
        pthread_mutex_unlock(&proxy->exchange_mutex);
        return MAELYS_MCP_ERR_MEMORY;
    }
    maelys_mcp_result_t status = proxy_write(proxy, request);
    json_decref(request);
    if (status != MAELYS_MCP_OK) {
        pthread_mutex_lock(&proxy->state_mutex);
        proxy->waiting = 0;
        if (proxy->progress_token) json_decref(proxy->progress_token);
        proxy->progress_token = NULL;
        set_proxy_failure_locked(proxy, status,
            "cannot write to the upstream MCP server");
        pthread_mutex_unlock(&proxy->state_mutex);
        pthread_mutex_unlock(&proxy->exchange_mutex);
        replace_error(out_error, "cannot write to the upstream MCP server");
        return status;
    }

    pthread_mutex_lock(&proxy->state_mutex);
    while (!proxy->pending_response && !proxy->failed) {
        /* Deliver whatever the reader queued, on this thread and outside the
         * lock: emit can block on outbox admission, and the reporter is only
         * ours to use. */
        if (proxy->pending_progress) {
            json_t *frames = proxy->pending_progress;
            proxy->pending_progress = NULL;
            pthread_mutex_unlock(&proxy->state_mutex);
            emit_pending_progress(reporter, frames);
            json_decref(frames);
            pthread_mutex_lock(&proxy->state_mutex);
            continue;
        }
        int waited = maelys_mcp_cond_wait_until(
            &proxy->response_ready, &proxy->state_mutex, deadline_ms);
        if (waited == ETIMEDOUT) {
            set_proxy_failure_locked(proxy, MAELYS_MCP_ERR_IO,
                "upstream MCP response deadline exceeded");
            proxy->closing = 1;
            break;
        }
        if (waited != 0) {
            set_proxy_failure_locked(proxy, MAELYS_MCP_ERR_IO,
                "cannot wait for the upstream MCP response");
            proxy->closing = 1;
            break;
        }
    }
    json_t *response = proxy->pending_response;
    proxy->pending_response = NULL;
    proxy->waiting = 0;
    if (proxy->progress_token) json_decref(proxy->progress_token);
    proxy->progress_token = NULL;
    /* Take whatever progress is still queued along with the response: the loop
     * above exits the moment pending_response is set, and those frames must
     * still go out ahead of the result. */
    json_t *trailing_progress = proxy->pending_progress;
    proxy->pending_progress = NULL;
    maelys_mcp_result_t failure_status = proxy->failure_status;
    char *failure_message = maelys_mcp_strdup(proxy->failure_message);
    int closing = proxy->closing;
    pthread_mutex_unlock(&proxy->state_mutex);
    if (trailing_progress) {
        emit_pending_progress(reporter, trailing_progress);
        json_decref(trailing_progress);
    }
    if (closing) (void)shutdown(proxy->fd, SHUT_RDWR);
    if (!response) {
        replace_error(out_error, failure_message ? failure_message :
            "upstream MCP transport failed");
        free(failure_message);
        pthread_mutex_unlock(&proxy->exchange_mutex);
        return failure_status ? failure_status : MAELYS_MCP_ERR_PROVIDER;
    }
    free(failure_message);
    json_t *error = json_object_get(response, "error");
    json_t *result = json_object_get(response, "result");
    if (!maelys_mcp_json_string_equals(json_object_get(response, "jsonrpc"), "2.0") ||
        !!error == !!result || (error && !json_is_object(error))) {
        json_decref(response);
        replace_error(out_error,
            "upstream MCP response must contain exactly one result or error");
        pthread_mutex_unlock(&proxy->exchange_mutex);
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    if (error) {
        json_t *message = json_object_get(error, "message");
        replace_error(out_error, json_is_string(message) &&
            !maelys_mcp_json_string_has_nul(message) ?
            json_string_value(message) : "upstream MCP call failed");
        json_decref(response);
        pthread_mutex_unlock(&proxy->exchange_mutex);
        return MAELYS_MCP_ERR_PROVIDER;
    }
    *out_result = json_incref(result);
    json_decref(response);
    pthread_mutex_unlock(&proxy->exchange_mutex);
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t proxy_notify(
    proxy_context_t *proxy,
    const char *method,
    json_t *params) {
    json_t *notification = json_object();
    if (!notification || !params) {
        if (notification) json_decref(notification);
        if (params) json_decref(params);
        return MAELYS_MCP_ERR_MEMORY;
    }
    if (json_object_set_new(notification, "jsonrpc", json_string("2.0")) != 0 ||
        json_object_set_new(notification, "method", json_string(method)) != 0 ||
        json_object_set(notification, "params", params) != 0) {
        json_decref(notification);
        json_decref(params);
        return MAELYS_MCP_ERR_MEMORY;
    }
    json_decref(params);
    maelys_mcp_result_t status = proxy_write(proxy, notification);
    json_decref(notification);
    return status;
}

static json_t *modern_meta(void) {
    return json_pack("{s:s,s:{s:s,s:s},s:{}}",
        "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
        "io.modelcontextprotocol/clientInfo",
            "name", "maelys-mcp-proxy",
            "version", maelys_mcp_version_string(),
        "io.modelcontextprotocol/clientCapabilities");
}

/* Params carrying whatever the negotiated era requires on every request:
 * the modern triple in _meta, nothing at all for legacy. */
static json_t *proxy_request_params(const proxy_context_t *proxy) {
    json_t *params = json_object();
    if (!params) return NULL;
    /* set_new consumes the reference whether or not it succeeds, so meta stops
     * being ours the moment it is handed over. */
    if (proxy->era == PROXY_ERA_MODERN) {
        json_t *meta = modern_meta();
        if (!meta) {
            json_decref(params);
            return NULL;
        }
        if (json_object_set_new(params, "_meta", meta) != 0) {
            json_decref(params);
            return NULL;
        }
    }
    return params;
}

static const char *lookup_binding(
    const proxy_context_t *proxy,
    const char *exposed_name) {
    if (!exposed_name) return NULL;
    for (size_t index = 0; index < proxy->binding_count; ++index) {
        if (strcmp(proxy->bindings[index].exposed_name, exposed_name) == 0) {
            return proxy->bindings[index].upstream_name;
        }
    }
    return NULL;
}

static maelys_mcp_result_t proxy_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    proxy_context_t *proxy = context;
    /*
     * The TOCTOU rule from docs/middleware-design.md: resolve against the
     * pinned snapshot and never re-list. A name that is not in it is refused
     * here, before any byte reaches the upstream.
     */
    const char *upstream_name = lookup_binding(proxy, request->tool_name);
    if (!upstream_name) {
        replace_error(out_error,
            "tool is not in the pinned upstream catalog snapshot");
        return MAELYS_MCP_ERR_NOT_FOUND;
    }
    /* v1 does not forward MRTR continuations: the upstream is a separate
     * conversation, and its requestState is not ours to round-trip. */
    if (request->input_responses || request->request_state) {
        replace_error(out_error,
            "the MCP proxy does not forward MRTR continuations");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    json_t *params = proxy_request_params(proxy);
    if (!params) return MAELYS_MCP_ERR_MEMORY;
    if (json_object_set_new(params, "name", json_string(upstream_name)) != 0 ||
        json_object_set(params, "arguments", request->arguments) != 0) {
        json_decref(params);
        return MAELYS_MCP_ERR_MEMORY;
    }
    json_t *result = NULL;
    maelys_mcp_result_t status = proxy_exchange(proxy, "tools/call", params,
        proxy->call_timeout_ms, request->progress != NULL, request->progress,
        &result, out_error);
    if (status != MAELYS_MCP_OK) return status;
    json_t *content = json_is_object(result) ?
        json_object_get(result, "content") : NULL;
    json_t *structured = json_is_object(result) ?
        json_object_get(result, "structuredContent") : NULL;
    json_t *is_error = json_is_object(result) ?
        json_object_get(result, "isError") : NULL;
    json_t *input_requests = json_is_object(result) ?
        json_object_get(result, "inputRequests") : NULL;
    if (!json_is_object(result) || (content && !json_is_array(content)) ||
        (structured && !json_is_object(structured)) ||
        (is_error && !json_is_boolean(is_error))) {
        json_decref(result);
        replace_error(out_error, "upstream MCP tool result has invalid types");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    if (input_requests || maelys_mcp_json_string_equals(
        json_object_get(result, "resultType"), "input_required")) {
        json_decref(result);
        replace_error(out_error,
            "upstream MCP server returned a continuation the proxy does not relay");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    if (!content && !structured) {
        json_decref(result);
        replace_error(out_error,
            "upstream MCP tool result has neither content nor structuredContent");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    out_result->type = MAELYS_MCP_PROVIDER_RESULT_COMPLETE;
    /* Content passes through as the upstream wrote it; the runtime validates
     * it again downstream through maelys_mcp_validate_content. */
    out_result->content = content ? json_deep_copy(content) : NULL;
    out_result->structured_content = structured ? json_deep_copy(structured) : NULL;
    out_result->is_error = json_is_true(is_error);
    if ((content && !out_result->content) ||
        (structured && !out_result->structured_content)) {
        maelys_mcp_provider_result_clear(out_result);
        json_decref(result);
        return MAELYS_MCP_ERR_MEMORY;
    }
    json_decref(result);
    return MAELYS_MCP_OK;
}

static void proxy_destroy(void *context) {
    proxy_context_t *proxy = context;
    if (!proxy) return;
    if (proxy->fd >= 0) {
        pthread_mutex_lock(&proxy->state_mutex);
        proxy->closing = 1;
        pthread_cond_broadcast(&proxy->response_ready);
        pthread_mutex_unlock(&proxy->state_mutex);
        /*
         * EOF on the upstream's stdin is how an MCP stdio server is asked to
         * exit. That half-close is this kind's protocol goodbye and stays
         * above the seam, exactly as the native provider's provider/shutdown
         * exchange does - they are protocol facts, not launch facts, and
         * unifying them would be wrong.
         *
         * The SHUT_RDWR immediately after is not a second goodbye: it belongs
         * to closing the transport, which is the ladder's first step, and is
         * what guarantees the reader unblocks so it can be joined. Nothing
         * separates the two because nothing should - the upstream's time to
         * exit is the ladder's grace budget, which is a bound the runtime
         * states, rather than a sleep nobody can account for.
         */
        (void)shutdown(proxy->fd, SHUT_WR);
        (void)shutdown(proxy->fd, SHUT_RDWR);
        if (proxy->reader_started) {
            (void)pthread_join(proxy->reader_thread, NULL);
            proxy->reader_started = 0;
        }
        close(proxy->fd);
        proxy->fd = -1;
    }
    /*
     * One bounded ladder for both provider kinds, and for both the
     * transport-up and transport-never-opened cases this teardown serves.
     * The containment diagnostic stops here: the destroy contract is void
     * from this frame all the way up, so there is no caller to hand it to -
     * tests/test_process_launcher.c pins the ladder's own return instead.
     */
    char *containment_error = NULL;
    (void)maelys_mcp_process_shutdown(&proxy->slot,
        PROXY_SHUTDOWN_TIMEOUT_MS, PROXY_SHUTDOWN_TIMEOUT_MS,
        &containment_error);
    free(containment_error);
    if (proxy->reader) {
        maelys_mcp_line_reader_clear(proxy->reader);
        free(proxy->reader);
    }
    for (size_t index = 0; index < proxy->binding_count; ++index) {
        free(proxy->bindings[index].exposed_name);
        free(proxy->bindings[index].upstream_name);
    }
    free(proxy->bindings);
    if (proxy->pending_response) json_decref(proxy->pending_response);
    if (proxy->pending_progress) json_decref(proxy->pending_progress);
    if (proxy->progress_token) json_decref(proxy->progress_token);
    free(proxy->failure_message);
    if (proxy->response_ready_initialized) {
        pthread_cond_destroy(&proxy->response_ready);
    }
    if (proxy->state_mutex_initialized) {
        pthread_mutex_destroy(&proxy->state_mutex);
    }
    if (proxy->write_mutex_initialized) {
        pthread_mutex_destroy(&proxy->write_mutex);
    }
    if (proxy->exchange_mutex_initialized) {
        pthread_mutex_destroy(&proxy->exchange_mutex);
    }
    free(proxy);
}

/*
 * `execution_profile` is manifest v2's "executionProfile" (docs/manifest.md),
 * opaque pass-through to the launcher; NULL for every caller that carries
 * none, which reproduces this function's pre-v2 behaviour.
 */
static maelys_mcp_result_t spawn_upstream(
    const maelys_mcp_proxy_options_t *options,
    const char *execution_profile,
    maelys_mcp_process_launcher_t *launcher,
    proxy_context_t **out_proxy,
    char **out_error) {
    /*
     * The upstream's argv is the caller's whole vector, not supplements: an
     * MCP server has an arbitrary CLI (--stdio, serve, a subcommand, a script
     * path) and the runtime holds no invariant about how a third-party server
     * is invoked. That is the deliberate asymmetry with the native provider,
     * where the runtime does hold one and compiles argv itself. Either way the
     * launcher receives a complete vector.
     */
    char *const fallback_argv[] = {(char *)options->executable_path, NULL};
    maelys_mcp_process_launch_params_t params = {
        .executable_path = options->executable_path,
        .argv = options->argv ? options->argv : fallback_argv,
        .execution_profile = execution_profile,
        /* One connect budget covers spawn, era negotiation and the single
         * tools/list, so the launch's share of it is the whole thing. */
        .spawn_timeout_ms = options->connect_timeout_ms,
        .grace_timeout_ms = PROXY_SHUTDOWN_TIMEOUT_MS,
        .force_timeout_ms = PROXY_SHUTDOWN_TIMEOUT_MS,
        /*
         * STDIO, necessarily, and structurally rather than by policy: an
         * mcp-proxy upstream is a third-party MCP server that speaks stdin and
         * stdout by specification and will never understand a protocol
         * descriptor on fd 3. maelys_mcp_proxy_options_t exposes no layout
         * field, so this kind cannot be given anything else even by mistake -
         * and because the layout is what decides MAELYS_PROVIDER_FD, this kind
         * cannot be handed that variable by mistake either.
         */
        .fd_layout = MAELYS_MCP_PROCESS_FD_STDIO
    };
    maelys_mcp_process_slot_t slot = {
        .launcher = NULL, .handle = NULL, .protocol_fd = -1, .live = 0
    };
    maelys_mcp_result_t launch_status = maelys_mcp_process_launch(launcher,
        &params, &slot, out_error);
    if (launch_status != MAELYS_MCP_OK) return launch_status;
    proxy_context_t *proxy = calloc(1u, sizeof(*proxy));
    if (!proxy) {
        close(slot.protocol_fd);
        /* Forced, with no graceful rung: the child is seconds old and has
         * produced nothing to flush, and this is a failure of the runtime's
         * own rather than a shutdown the upstream deserves notice of. */
        maelys_mcp_process_abandon(&slot, PROXY_SHUTDOWN_TIMEOUT_MS);
        return MAELYS_MCP_ERR_MEMORY;
    }
    proxy->fd = -1;
    proxy->slot = slot;
    proxy->max_message_bytes = options->max_message_bytes;
    proxy->call_timeout_ms = options->call_timeout_ms;
    proxy->reader = calloc(1u, sizeof(*proxy->reader));
    maelys_mcp_result_t status = MAELYS_MCP_ERR_MEMORY;
    if (!proxy->reader || maelys_mcp_line_reader_init(
        proxy->reader, options->max_message_bytes) != MAELYS_MCP_OK) goto failed;
    status = MAELYS_MCP_ERR_IO;
    if (pthread_mutex_init(&proxy->exchange_mutex, NULL) != 0) goto failed;
    proxy->exchange_mutex_initialized = 1;
    if (pthread_mutex_init(&proxy->write_mutex, NULL) != 0) goto failed;
    proxy->write_mutex_initialized = 1;
    if (pthread_mutex_init(&proxy->state_mutex, NULL) != 0) goto failed;
    proxy->state_mutex_initialized = 1;
    if (maelys_mcp_cond_init_monotonic(&proxy->response_ready) != 0) goto failed;
    proxy->response_ready_initialized = 1;
    proxy->fd = slot.protocol_fd;
    if (pthread_create(&proxy->reader_thread, NULL, proxy_reader_main, proxy) != 0) {
        goto failed;
    }
    proxy->reader_started = 1;
    *out_proxy = proxy;
    return MAELYS_MCP_OK;
failed:
    if (proxy->fd < 0) close(slot.protocol_fd);
    /*
     * Abandoned rather than shut down: nothing has spoken to this upstream
     * yet, so it is owed no goodbye and no graceful rung. Doing it here also
     * leaves the slot not live, which makes the ladder inside proxy_destroy
     * the no-op it should be - release still reaches the launcher exactly
     * once, from exactly one place, and so does the drop of the reference this
     * provider took on it.
     */
    maelys_mcp_process_abandon(&proxy->slot, PROXY_SHUTDOWN_TIMEOUT_MS);
    replace_error(out_error, "cannot start the upstream MCP transport");
    proxy_destroy(proxy);
    return status;
}

/*
 * Modern first. A valid discover result settles the era; a JSON-RPC error, or
 * an answer that is not a discover result, means the upstream predates
 * 2026-07-28 and the legacy handshake is tried instead. A transport failure is
 * not a fallback signal - there is nothing left to hand the handshake to.
 */
static maelys_mcp_result_t negotiate_era(
    proxy_context_t *proxy,
    uint64_t deadline_ms,
    char **out_error) {
    json_t *params = json_object();
    json_t *meta = modern_meta();
    if (!params || !meta) {
        if (params) json_decref(params);
        if (meta) json_decref(meta);
        return MAELYS_MCP_ERR_MEMORY;
    }
    if (json_object_set_new(params, "_meta", meta) != 0) {
        json_decref(params);
        return MAELYS_MCP_ERR_MEMORY;
    }
    json_t *result = NULL;
    maelys_mcp_result_t status = proxy_exchange(proxy, "server/discover", params,
        remaining_ms(deadline_ms), 0, NULL, &result, out_error);
    if (status == MAELYS_MCP_OK) {
        json_t *versions = json_is_object(result) ?
            json_object_get(result, "supportedVersions") : NULL;
        int modern = 0;
        size_t index;
        json_t *version;
        json_array_foreach(versions, index, version) {
            if (maelys_mcp_json_string_equals(version, MAELYS_MCP_PROTOCOL_MODERN)) {
                modern = 1;
            }
        }
        json_decref(result);
        if (modern) {
            proxy->era = PROXY_ERA_MODERN;
            return MAELYS_MCP_OK;
        }
        replace_error(out_error,
            "upstream MCP server/discover did not advertise " MAELYS_MCP_PROTOCOL_MODERN);
    } else if (status != MAELYS_MCP_ERR_PROVIDER) {
        return status;
    }

    params = json_pack("{s:s,s:{},s:{s:s,s:s}}",
        "protocolVersion", MAELYS_MCP_PROTOCOL_LEGACY,
        "capabilities",
        "clientInfo",
            "name", "maelys-mcp-proxy",
            "version", maelys_mcp_version_string());
    if (!params) return MAELYS_MCP_ERR_MEMORY;
    result = NULL;
    status = proxy_exchange(proxy, "initialize", params,
        remaining_ms(deadline_ms), 0, NULL, &result, out_error);
    if (status != MAELYS_MCP_OK) return status;
    /* Accept whatever supported dated revision the upstream echoes - MCP
     * version negotiation lets a server answer with a different one. */
    json_t *version = json_is_object(result) ?
        json_object_get(result, "protocolVersion") : NULL;
    if (!json_is_string(version) || maelys_mcp_json_string_has_nul(version) ||
        json_string_length(version) == 0u) {
        json_decref(result);
        replace_error(out_error,
            "upstream MCP initialize result has no protocolVersion");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    json_decref(result);
    proxy->era = PROXY_ERA_LEGACY;
    status = proxy_notify(proxy, "notifications/initialized", json_object());
    if (status != MAELYS_MCP_OK) {
        replace_error(out_error,
            "cannot complete the upstream MCP initialize handshake");
    }
    return status;
}

static char *join_prefix(const char *prefix, const char *name) {
    size_t prefix_length = prefix ? strlen(prefix) : 0u;
    size_t name_length = strlen(name);
    char *joined = malloc(prefix_length + name_length + 1u);
    if (!joined) return NULL;
    if (prefix_length) memcpy(joined, prefix, prefix_length);
    memcpy(joined + prefix_length, name, name_length + 1u);
    return joined;
}

/* Grows *list into "a,b,c" as each skipped tool is found. *list starts NULL
 * (nothing skipped yet) and stays the caller's to free either way. */
static maelys_mcp_result_t append_skipped_tool(char **list, const char *name) {
    size_t existing = *list ? strlen(*list) : 0u;
    size_t addition = strlen(name);
    char *grown = realloc(*list, existing + (existing ? 1u : 0u) + addition + 1u);
    if (!grown) return MAELYS_MCP_ERR_MEMORY;
    if (existing) grown[existing++] = ',';
    memcpy(grown + existing, name, addition + 1u);
    *list = grown;
    return MAELYS_MCP_OK;
}

/*
 * The catalog snapshot. Listed once, at connect, and never again: this array
 * is the pinned identity every later call is resolved against.
 *
 * schema_policy governs a tool whose inputSchema this runtime cannot
 * validate: STRICT fails the whole connect (see docs/mcp-proxy.md), SKIP
 * drops just that tool and records its upstream name in *out_skipped_tools,
 * PASSTHROUGH keeps the tool but exposes it with the permissive
 * {"type":"object"} schema instead. A tool whose schema does validate is
 * unaffected by the policy either way.
 */
static maelys_mcp_result_t snapshot_catalog(
    proxy_context_t *proxy,
    const char *tool_prefix,
    maelys_mcp_proxy_schema_policy_t schema_policy,
    uint64_t deadline_ms,
    json_t **out_listing,
    char **out_skipped_tools,
    char **out_error) {
    json_t *params = proxy_request_params(proxy);
    if (!params) return MAELYS_MCP_ERR_MEMORY;
    json_t *result = NULL;
    maelys_mcp_result_t status = proxy_exchange(proxy, "tools/list", params,
        remaining_ms(deadline_ms), 0, NULL, &result, out_error);
    if (status != MAELYS_MCP_OK) return status;
    json_t *tools = json_is_object(result) ? json_object_get(result, "tools") : NULL;
    if (!json_is_array(tools)) {
        json_decref(result);
        replace_error(out_error, "upstream MCP tools/list result has no tools array");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    size_t count = json_array_size(tools);
    proxy->bindings = count ? calloc(count, sizeof(*proxy->bindings)) : NULL;
    if (count && !proxy->bindings) {
        json_decref(result);
        return MAELYS_MCP_ERR_MEMORY;
    }
    size_t kept = 0u;
    for (size_t index = 0; index < count; ++index) {
        json_t *tool = json_array_get(tools, index);
        json_t *name = json_is_object(tool) ? json_object_get(tool, "name") : NULL;
        json_t *title = json_is_object(tool) ? json_object_get(tool, "title") : NULL;
        json_t *description = json_is_object(tool) ?
            json_object_get(tool, "description") : NULL;
        json_t *input_schema = json_is_object(tool) ?
            json_object_get(tool, "inputSchema") : NULL;
        if (!json_is_string(name) || maelys_mcp_json_string_has_nul(name) ||
            json_string_length(name) == 0u ||
            (title && (!json_is_string(title) || maelys_mcp_json_string_has_nul(title))) ||
            (description && (!json_is_string(description) ||
                maelys_mcp_json_string_has_nul(description))) ||
            !json_is_object(input_schema)) {
            json_decref(result);
            replace_error(out_error, "upstream MCP tool description is invalid");
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        char *schema_error = NULL;
        maelys_mcp_result_t schema_status = maelys_mcp_validate_schema_definition(
            input_schema, 1, &schema_error);
        int passthrough = 0;
        if (schema_status != MAELYS_MCP_OK) {
            if (schema_policy == MAELYS_MCP_PROXY_SCHEMA_STRICT) {
                /*
                 * A schema this runtime cannot validate fails the connect
                 * rather than being dropped from the catalog: a silently
                 * shorter tool list is the kind of difference nobody notices
                 * until a call goes missing. This is the default - an unset
                 * schema_policy field keeps this behaviour.
                 */
                json_decref(result);
                replace_error(out_error, schema_error ?
                    schema_error : "upstream MCP tool schema is unsupported");
                free(schema_error);
                return MAELYS_MCP_ERR_PROTOCOL;
            }
            free(schema_error);
            if (schema_policy == MAELYS_MCP_PROXY_SCHEMA_SKIP) {
                if (append_skipped_tool(out_skipped_tools,
                    json_string_value(name)) != MAELYS_MCP_OK) {
                    json_decref(result);
                    return MAELYS_MCP_ERR_MEMORY;
                }
                continue;
            }
            /* MAELYS_MCP_PROXY_SCHEMA_PASSTHROUGH: keep the tool, but the
             * catalog will expose {"type":"object"} for it instead of the
             * schema that failed validation. */
            passthrough = 1;
        } else {
            free(schema_error);
        }
        proxy_binding_t *binding = &proxy->bindings[kept];
        binding->upstream_name = maelys_mcp_strdup(json_string_value(name));
        binding->exposed_name = join_prefix(tool_prefix, json_string_value(name));
        binding->source_index = index;
        binding->passthrough = passthrough;
        ++kept;
        proxy->binding_count = kept;
        if (!binding->upstream_name || !binding->exposed_name) {
            json_decref(result);
            return MAELYS_MCP_ERR_MEMORY;
        }
    }
    proxy->binding_count = kept;
    *out_listing = result;
    return MAELYS_MCP_OK;
}

maelys_mcp_result_t maelys_mcp_provider_proxy_spawn(
    const maelys_mcp_proxy_options_t *options,
    maelys_mcp_provider_t **out_provider,
    char **out_skipped_tools,
    char **out_error) {
    /* The API that predates the seam, unchanged in behaviour: it binds the
     * launcher that reproduces what it always did, and releases it. The
     * provider took its own reference if the spawn succeeded. */
    maelys_mcp_process_launcher_t *launcher = NULL;
    maelys_mcp_result_t status = maelys_mcp_posix_launcher_create(&launcher,
        out_error);
    if (status != MAELYS_MCP_OK) {
        if (out_skipped_tools) *out_skipped_tools = NULL;
        return status;
    }
    status = maelys_mcp_provider_proxy_spawn_with_launcher(options, launcher,
        out_provider, out_skipped_tools, out_error);
    maelys_mcp_process_launcher_release(launcher);
    return status;
}

/*
 * The shared body behind every proxy spawn entry point. `execution_profile`
 * is manifest v2's "executionProfile" (docs/manifest.md); the pre-v2 entry
 * points pass NULL, which reproduces their exact pre-v2 behaviour through
 * spawn_upstream.
 */
static maelys_mcp_result_t provider_proxy_spawn_with_launcher_and_profile(
    const maelys_mcp_proxy_options_t *options,
    const char *execution_profile,
    maelys_mcp_process_launcher_t *launcher,
    maelys_mcp_provider_t **out_provider,
    char **out_skipped_tools,
    char **out_error) {
    if (out_skipped_tools) *out_skipped_tools = NULL;
    if (!launcher) {
        replace_error(out_error, "upstream MCP launcher must not be null");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    if (!options || !options->executable_path ||
        options->executable_path[0] != '/' || !out_provider ||
        (options->tool_prefix && !*options->tool_prefix)) {
        replace_error(out_error,
            "upstream MCP executable must be an absolute path");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    maelys_mcp_proxy_options_t normalized = *options;
    if (!normalized.max_message_bytes) {
        normalized.max_message_bytes = MAELYS_MCP_DEFAULT_MAX_MESSAGE_BYTES;
    }
    if (!normalized.connect_timeout_ms) {
        normalized.connect_timeout_ms = MAELYS_MCP_DEFAULT_PROXY_CONNECT_TIMEOUT_MS;
    }
    if (!normalized.call_timeout_ms) {
        normalized.call_timeout_ms = MAELYS_MCP_DEFAULT_PROVIDER_CALL_TIMEOUT_MS;
    }
    /*
     * MCP has no effect classes, so every upstream tool gets the caller's
     * default. UNSPECIFIED becomes EXECUTE rather than READ: a host gates
     * apply/commit/execute behind --allow-effect, so the fallback has to be a
     * gated class - an ungated default would turn "the caller said nothing"
     * into "anyone may call it".
     *
     * Inferring an effect from MCP's readOnlyHint/destructiveHint annotations
     * is a plausible follow-up, but those hints are advisory and
     * upstream-controlled, so v1 does not let them decide a policy input.
     */
    if (normalized.default_effect == MAELYS_MCP_EFFECT_UNSPECIFIED) {
        normalized.default_effect = MAELYS_MCP_EFFECT_EXECUTE;
    }
    if (!maelys_mcp_tool_effect_string(normalized.default_effect)) {
        replace_error(out_error, "default effect is not a known effect class");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    *out_provider = NULL;

    uint64_t deadline_ms = 0u;
    if (maelys_mcp_monotonic_deadline(
        normalized.connect_timeout_ms, &deadline_ms) != 0) {
        replace_error(out_error, "cannot compute the upstream MCP connect deadline");
        return MAELYS_MCP_ERR_IO;
    }
    proxy_context_t *proxy = NULL;
    maelys_mcp_result_t status = spawn_upstream(&normalized, execution_profile,
        launcher, &proxy, out_error);
    if (status != MAELYS_MCP_OK) return status;
    status = negotiate_era(proxy, deadline_ms, out_error);
    if (status != MAELYS_MCP_OK) {
        proxy_destroy(proxy);
        return status;
    }
    json_t *listing = NULL;
    char *skipped_tools = NULL;
    status = snapshot_catalog(proxy, normalized.tool_prefix,
        normalized.schema_policy, deadline_ms, &listing, &skipped_tools, out_error);
    if (status != MAELYS_MCP_OK) {
        free(skipped_tools);
        proxy_destroy(proxy);
        return status;
    }
    json_t *tools_json = json_object_get(listing, "tools");
    size_t count = proxy->binding_count;
    maelys_mcp_tool_t *tools = count ? calloc(count, sizeof(*tools)) : NULL;
    if (count && !tools) {
        free(skipped_tools);
        json_decref(listing);
        proxy_destroy(proxy);
        return MAELYS_MCP_ERR_MEMORY;
    }
    /* Shared by every MAELYS_MCP_PROXY_SCHEMA_PASSTHROUGH tool. One ref of
     * our own, released after maelys_mcp_provider_create has taken its own
     * (via json_incref) for each tool that points at it. */
    json_t *permissive_schema = NULL;
    for (size_t index = 0; index < count; ++index) {
        const proxy_binding_t *binding = &proxy->bindings[index];
        json_t *tool = json_array_get(tools_json, binding->source_index);
        json_t *title = json_object_get(tool, "title");
        json_t *description = json_object_get(tool, "description");
        json_t *schema = json_object_get(tool, "inputSchema");
        if (binding->passthrough) {
            if (!permissive_schema) permissive_schema = json_pack("{s:s}", "type", "object");
            schema = permissive_schema;
        }
        if (!schema) {
            free(skipped_tools);
            json_decref(listing);
            if (permissive_schema) json_decref(permissive_schema);
            free(tools);
            proxy_destroy(proxy);
            return MAELYS_MCP_ERR_MEMORY;
        }
        tools[index] = (maelys_mcp_tool_t){
            .name = binding->exposed_name,
            .title = json_is_string(title) ? json_string_value(title) :
                binding->exposed_name,
            .description = json_is_string(description) ?
                json_string_value(description) : "",
            .input_schema = schema,
            /* No outputSchema is republished: this proxy passes the upstream
             * result through, and advertising a schema it does not itself
             * enforce would claim a guarantee it cannot make. */
            .output_schema = NULL,
            .effect = normalized.default_effect
        };
    }
    maelys_mcp_provider_config_t config = {
        .name = "mcp-proxy",
        .version = maelys_mcp_version_string(),
        .tools = tools,
        .tool_count = count,
        .call = proxy_call,
        .destroy = proxy_destroy,
        .context = proxy
    };
    status = maelys_mcp_provider_create(&config, out_provider);
    free(tools);
    if (permissive_schema) json_decref(permissive_schema);
    json_decref(listing);
    if (status != MAELYS_MCP_OK) {
        free(skipped_tools);
        replace_error(out_error,
            "upstream MCP catalog cannot be republished by this runtime");
        proxy_destroy(proxy);
        return status;
    }
    if (out_skipped_tools) *out_skipped_tools = skipped_tools;
    else free(skipped_tools);
    return status;
}

maelys_mcp_result_t maelys_mcp_provider_proxy_spawn_with_launcher(
    const maelys_mcp_proxy_options_t *options,
    maelys_mcp_process_launcher_t *launcher,
    maelys_mcp_provider_t **out_provider,
    char **out_skipped_tools,
    char **out_error) {
    /* No manifest v2 data: byte-identical to what this entry point always
     * did. */
    return provider_proxy_spawn_with_launcher_and_profile(options, NULL,
        launcher, out_provider, out_skipped_tools, out_error);
}

maelys_mcp_result_t maelys_mcp_provider_proxy_spawn_with_profile(
    const maelys_mcp_proxy_options_t *options,
    const char *execution_profile,
    maelys_mcp_provider_t **out_provider,
    char **out_skipped_tools,
    char **out_error) {
    /* Always the POSIX launcher, like maelys_mcp_provider_proxy_spawn: this
     * entry point exists to carry manifest v2 data, not to name a launcher.
     * It binds one of its own and releases it. */
    maelys_mcp_process_launcher_t *launcher = NULL;
    maelys_mcp_result_t status = maelys_mcp_posix_launcher_create(&launcher,
        out_error);
    if (status != MAELYS_MCP_OK) {
        if (out_skipped_tools) *out_skipped_tools = NULL;
        return status;
    }
    status = provider_proxy_spawn_with_launcher_and_profile(options,
        execution_profile, launcher, out_provider, out_skipped_tools,
        out_error);
    maelys_mcp_process_launcher_release(launcher);
    return status;
}
