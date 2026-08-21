#include "src/internal/internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static void replace_error(char **out_error, const char *message) {
    if (!out_error) return;
    free(*out_error);
    *out_error = maelys_mcp_strdup(message ? message : "provider error");
}

static maelys_mcp_result_t configure_send_timeout(int fd, unsigned int timeout_ms) {
    struct timeval timeout = {
        .tv_sec = (time_t)(timeout_ms / 1000u),
        .tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u)
    };
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        return MAELYS_MCP_ERR_IO;
    }
    return MAELYS_MCP_OK;
}

static void set_process_failure_locked(
    maelys_mcp_process_context_t *process,
    maelys_mcp_result_t status,
    const char *message) {
    if (process->failed) return;
    process->failed = 1;
    process->failure_status = status;
    process->failure_message = maelys_mcp_strdup(
        message ? message : "provider transport failed");
    pthread_cond_broadcast(&process->response_ready);
    /*
     * A thread blocked on a nested client request is not waiting on this
     * condition - it is waiting on the channel, for a reply from the client -
     * so broadcasting alone would leave it there until the nested deadline,
     * holding an operation reference on a channel whose provider is already
     * dead. Reaching across to settle it is the one place the lock hierarchy
     * runs state_mutex -> channel->mutex; see src/internal/internal.h for why
     * that cannot close into a cycle.
     */
    if (process->nested_channel) {
        maelys_mcp_channel_nested_fail_id(process->nested_channel,
            process->nested_wait_id, MAELYS_MCP_ERR_PROVIDER);
    }
}

/*
 * Published by the thread inside the call for exactly the window it is blocked
 * on the client, so the failure path above knows what to cancel. Binding while
 * the provider is already dead settles the wait immediately rather than
 * letting it start.
 */
static void process_bind_nested_wait(
    void *context,
    maelys_mcp_channel_t *channel,
    const char *nested_id) {
    maelys_mcp_process_context_t *process = context;
    if (!process) return;
    pthread_mutex_lock(&process->state_mutex);
    process->nested_channel = channel;
    (void)snprintf(process->nested_wait_id, sizeof(process->nested_wait_id),
        "%s", nested_id ? nested_id : "");
    int already_failed = channel != NULL && (process->failed || process->closing);
    pthread_mutex_unlock(&process->state_mutex);
    if (already_failed) {
        maelys_mcp_channel_nested_fail_id(channel, nested_id,
            MAELYS_MCP_ERR_PROVIDER);
    }
}

static int provider_event_from_message(
    json_t *message,
    maelys_mcp_provider_event_t *out_event) {
    json_t *method = json_is_object(message) ? json_object_get(message, "method") : NULL;
    json_t *params = json_is_object(message) ? json_object_get(message, "params") : NULL;
    if (!json_is_string(method) || maelys_mcp_json_string_has_nul(method) ||
        (params && !json_is_object(params))) return 0;
    memset(out_event, 0, sizeof(*out_event));
    if (maelys_mcp_json_string_equals(method,
        "provider/notifications/resources/updated")) {
        json_t *uri = json_is_object(params) ? json_object_get(params, "uri") : NULL;
        if (!json_is_string(uri) || maelys_mcp_json_string_has_nul(uri)) return 0;
        out_event->kind = MAELYS_MCP_PROVIDER_EVENT_RESOURCE_UPDATED;
        out_event->resource_uri = json_string_value(uri);
        return 1;
    }
    if (params && json_object_size(params) != 0u) return 0;
    if (maelys_mcp_json_string_equals(method,
        "provider/notifications/resources/list_changed")) {
        out_event->kind = MAELYS_MCP_PROVIDER_EVENT_RESOURCES_LIST_CHANGED;
        return 1;
    }
    if (maelys_mcp_json_string_equals(method,
        "provider/notifications/tools/list_changed")) {
        out_event->kind = MAELYS_MCP_PROVIDER_EVENT_TOOLS_LIST_CHANGED;
        return 1;
    }
    return 0;
}

/*
 * Every version between the floor and the current one, oldest first, so the
 * index is the version's rank. The list matters as much as the endpoints: when
 * /4 shipped there were only two entries and "supported" could be spelled as
 * two comparisons, but a host that only accepted the floor and the newest
 * would start rejecting the /4 providers released against 0.13.0 the moment
 * /5 existed.
 */
static const char *const provider_protocols[] = {
    MAELYS_MCP_PROVIDER_PROTOCOL_FLOOR,
    "maelys-provider/4",
    MAELYS_MCP_PROVIDER_PROTOCOL
};

#define PROVIDER_PROTOCOL_COUNT \
    (sizeof(provider_protocols) / sizeof(provider_protocols[0]))

static int protocol_rank(const json_t *protocol) {
    for (size_t index = 0; index < PROVIDER_PROTOCOL_COUNT; ++index) {
        if (maelys_mcp_json_string_equals(protocol, provider_protocols[index])) {
            return (int)index;
        }
    }
    return -1;
}

static int protocol_rank_of(const char *protocol) {
    for (size_t index = 0; index < PROVIDER_PROTOCOL_COUNT; ++index) {
        if (strcmp(protocol, provider_protocols[index]) == 0) return (int)index;
    }
    return -1;
}

static int supported_provider_protocol(const json_t *protocol) {
    return protocol_rank(protocol) >= 0;
}

/*
 * The SDKs declare their own version rather than echoing ours, so any frame a
 * provider sends is enough to learn what it speaks and to stop addressing it
 * at the floor. Raise only: a provider that answered once at /5 does not get
 * to talk the host back down.
 */
static void raise_protocol_locked(
    maelys_mcp_process_context_t *process,
    int rank) {
    if (rank < 0 || rank <= protocol_rank_of(process->negotiated_protocol)) return;
    (void)snprintf(process->negotiated_protocol,
        sizeof(process->negotiated_protocol), "%s", provider_protocols[rank]);
}

/* Beyond this many undelivered frames the oldest are dropped. Progress is
 * advisory and monotonic, so a subset in order is still correct, whereas an
 * unbounded queue would hand a flooding provider a memory leak. */
#define MAX_PENDING_PROGRESS 256

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
            json_is_string(message) ? json_string_value(message) : NULL);
    }
}

static void *process_reader_main(void *opaque) {
    maelys_mcp_process_context_t *process = opaque;
    for (;;) {
        json_t *message = NULL;
        char *error = NULL;
        maelys_mcp_result_t status = maelys_mcp_line_reader_read(
            process->reader, process->fd, &message, &error);
        if (status != MAELYS_MCP_OK) {
            pthread_mutex_lock(&process->state_mutex);
            if (!process->closing) {
                set_process_failure_locked(process,
                    status == MAELYS_MCP_ERR_NOT_FOUND ?
                        MAELYS_MCP_ERR_PROVIDER : status,
                    error ? error : "provider transport closed");
            }
            pthread_mutex_unlock(&process->state_mutex);
            free(error);
            break;
        }
        json_t *protocol = json_is_object(message) ?
            json_object_get(message, "protocol") : NULL;
        json_t *id = json_is_object(message) ? json_object_get(message, "id") : NULL;
        int rank = protocol_rank(protocol);
        if (rank < 0) {
            json_decref(message);
            pthread_mutex_lock(&process->state_mutex);
            set_process_failure_locked(process, MAELYS_MCP_ERR_PROTOCOL,
                "provider returned an invalid protocol envelope");
            pthread_mutex_unlock(&process->state_mutex);
            break;
        }
        pthread_mutex_lock(&process->state_mutex);
        raise_protocol_locked(process, rank);
        pthread_mutex_unlock(&process->state_mutex);
        if (json_is_integer(id)) {
            unsigned long long response_id =
                (unsigned long long)json_integer_value(id);
            pthread_mutex_lock(&process->state_mutex);
            if (!process->waiting || process->pending_response ||
                response_id != process->expected_id) {
                set_process_failure_locked(process, MAELYS_MCP_ERR_PROTOCOL,
                    "provider returned an unexpected response id");
                pthread_mutex_unlock(&process->state_mutex);
                json_decref(message);
                break;
            }
            process->pending_response = message;
            pthread_cond_broadcast(&process->response_ready);
            pthread_mutex_unlock(&process->state_mutex);
            continue;
        }
        /* Progress is request-scoped, unlike the fanout events below: it is
         * queued for the thread inside the call rather than broadcast, so it
         * cannot reach a channel that did not ask for it. The provider never
         * names a token - the host holds it - so a provider cannot address
         * progress at a request that is not its own. */
        if (!id && maelys_mcp_json_string_equals(
                json_object_get(message, "method"),
                "provider/notifications/progress")) {
            json_t *frame = json_object_get(message, "params");
            pthread_mutex_lock(&process->state_mutex);
            if (process->waiting && json_is_object(frame)) {
                if (!process->pending_progress) process->pending_progress = json_array();
                if (process->pending_progress) {
                    while (json_array_size(process->pending_progress) >= MAX_PENDING_PROGRESS) {
                        (void)json_array_remove(process->pending_progress, 0);
                    }
                    /* Copied for the same reason the nested request below is:
                     * the reader releases its message as soon as it has
                     * queued this, and the thread that drains the queue must
                     * not be reading a node this one is still releasing. */
                    (void)json_array_append_new(process->pending_progress,
                        json_deep_copy(frame));
                    pthread_cond_broadcast(&process->response_ready);
                }
            }
            pthread_mutex_unlock(&process->state_mutex);
            json_decref(message);
            continue;
        }
        /*
         * The third demux branch, and the one nesting needed: "has a method,
         * has no id" is a provider-initiated request, distinct both from a
         * response (id present) and from the three whitelisted id-less events
         * below. It is handed to the thread inside the call - the reader must
         * never do the client round trip itself, or the connection would stop
         * being read for the duration of it.
         *
         * Two nested requests outstanding at once is fatal here rather than
         * queued: the provider wire is strictly single-outstanding in both
         * directions, and a provider that broke that has already lost track of
         * which reply answers which request.
         */
        if (!id && maelys_mcp_json_string_equals(
                json_object_get(message, "method"),
                "provider/nestedRequest")) {
            json_t *params = json_object_get(message, "params");
            pthread_mutex_lock(&process->state_mutex);
            int refused = !process->waiting || process->pending_nested ||
                process->nested_inflight || !json_is_object(params);
            if (refused) {
                set_process_failure_locked(process, MAELYS_MCP_ERR_PROTOCOL,
                    "provider issued an unexpected or overlapping nested request");
                pthread_mutex_unlock(&process->state_mutex);
                json_decref(message);
                break;
            }
            /*
             * A copy, so the reader and the thread that acts on this frame
             * share no node. A reference would leave the worker reading the
             * request's strings while this thread releases the message they
             * belong to, which is the same "two threads, one jansson node"
             * race the rest of this runtime is built to avoid - and the wake
             * below is precisely what lets those two run at once.
             */
            process->pending_nested = json_deep_copy(params);
            if (!process->pending_nested) {
                set_process_failure_locked(process, MAELYS_MCP_ERR_MEMORY,
                    "cannot take the provider's nested request");
                pthread_mutex_unlock(&process->state_mutex);
                json_decref(message);
                break;
            }
            pthread_cond_broadcast(&process->response_ready);
            pthread_mutex_unlock(&process->state_mutex);
            json_decref(message);
            continue;
        }
        maelys_mcp_provider_event_t event;
        if (id || !provider_event_from_message(message, &event)) {
            json_decref(message);
            pthread_mutex_lock(&process->state_mutex);
            set_process_failure_locked(process, MAELYS_MCP_ERR_PROTOCOL,
                "provider returned an invalid asynchronous event");
            pthread_mutex_unlock(&process->state_mutex);
            break;
        }
        pthread_mutex_lock(&process->state_mutex);
        while (process->activation_pending && !process->events_enabled &&
            !process->failed && !process->closing) {
            pthread_cond_wait(&process->response_ready, &process->state_mutex);
        }
        maelys_mcp_provider_t *owner = process->owner;
        int events_enabled = process->events_enabled;
        pthread_mutex_unlock(&process->state_mutex);
        if (!owner || !events_enabled) {
            json_decref(message);
            pthread_mutex_lock(&process->state_mutex);
            set_process_failure_locked(process, MAELYS_MCP_ERR_PROTOCOL,
                "provider emitted an event before activation");
            pthread_mutex_unlock(&process->state_mutex);
            break;
        }
        (void)maelys_mcp_provider_emit_event(owner, &event);
        json_decref(message);
    }
    return NULL;
}

/* The error a nested request comes back with, in the provider wire's own
 * string-code convention (the same one `not_found` already uses). */
static const char *nested_error_code(maelys_mcp_result_t status) {
    switch (status) {
        case MAELYS_MCP_ERR_PROVIDER: return "client_error";
        case MAELYS_MCP_ERR_DENIED: return "denied";
        case MAELYS_MCP_ERR_TIMEOUT: return "timeout";
        case MAELYS_MCP_ERR_CLOSED: return "cancelled";
        case MAELYS_MCP_ERR_STATE: return "unavailable";
        default: return "failed";
    }
}

/*
 * json_object_set_new consumes its value whether it succeeds or fails, so
 * every handover below stops owning the value on the same line it is handed
 * over - not one branch later, which is where a failing sibling condition
 * would otherwise turn a leak into a double release.
 */
static json_t *nested_reply_message(
    const char *protocol,
    const char *nested_id,
    maelys_mcp_result_t status,
    json_t *payload,
    const char *message_text) {
    json_t *params = json_object();
    json_t *reply = json_object();
    if (!params || !reply) goto failed;
    if (json_object_set_new(params, "nestedId", json_string(nested_id)) != 0) {
        goto failed;
    }
    if (status == MAELYS_MCP_OK) {
        json_t *result = payload ? json_incref(payload) : json_object();
        if (!result) goto failed;
        if (json_object_set_new(params, "result", result) != 0) goto failed;
    } else {
        json_t *error = json_object();
        if (!error) goto failed;
        int described = json_object_set_new(error, "code",
                json_string(nested_error_code(status))) == 0 &&
            json_object_set_new(error, "message",
                json_string(message_text ? message_text :
                    "nested request failed")) == 0 &&
            /* The client's own error object travels as data, so a provider can
             * tell "the user declined" from "the request never got there". */
            (!payload || json_object_set(error, "data", payload) == 0);
        int attached = json_object_set_new(params, "error", error) == 0;
        if (!described || !attached) goto failed;
    }
    if (json_object_set_new(reply, "protocol", json_string(protocol)) != 0 ||
        json_object_set_new(reply, "method",
            json_string("provider/nestedReply")) != 0) {
        goto failed;
    }
    int attached = json_object_set_new(reply, "params", params) == 0;
    params = NULL;
    if (!attached) goto failed;
    return reply;
failed:
    if (params) json_decref(params);
    if (reply) json_decref(reply);
    return NULL;
}

static void shift_deadline(struct timespec *deadline, long long elapsed_ms) {
    if (elapsed_ms <= 0) return;
    deadline->tv_sec += (time_t)(elapsed_ms / 1000LL);
    long nanoseconds = deadline->tv_nsec + (long)(elapsed_ms % 1000LL) * 1000000L;
    deadline->tv_sec += nanoseconds / 1000000000L;
    deadline->tv_nsec = nanoseconds % 1000000000L;
}

static long long realtime_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return -1;
    return (long long)now.tv_sec * 1000LL + (long long)now.tv_nsec / 1000000LL;
}

/*
 * Runs one nested round trip on the thread that owns the call, then writes the
 * correlated reply back through the exchange mutex that thread already holds -
 * so no second write path exists, and the reply cannot interleave with another
 * request's bytes.
 *
 * The call deadline is shifted by however long the client took. A tool call
 * has 300 seconds; a person answering an elicitation may take longer than
 * that, and charging their thinking time against the provider's deadline would
 * make the timeout fire on the one party that did nothing wrong.
 */
static void relay_nested_request(
    maelys_mcp_process_context_t *process,
    maelys_mcp_nested_relay_t *relay,
    json_t *params,
    struct timespec *deadline) {
    json_t *nested_id = json_object_get(params, "nestedId");
    json_t *method = json_object_get(params, "method");
    json_t *inner = json_object_get(params, "params");
    if (!json_is_string(nested_id) || maelys_mcp_json_string_has_nul(nested_id) ||
        !json_is_string(method) || maelys_mcp_json_string_has_nul(method) ||
        (inner && !json_is_object(inner))) {
        pthread_mutex_lock(&process->state_mutex);
        set_process_failure_locked(process, MAELYS_MCP_ERR_PROTOCOL,
            "provider sent a malformed nested request");
        pthread_mutex_unlock(&process->state_mutex);
        return;
    }
    long long started_ms = realtime_milliseconds();
    json_t *result = NULL;
    char *error = NULL;
    maelys_mcp_result_t status = relay ?
        maelys_mcp_provider_request_client(relay, json_string_value(method),
            inner, &result, &error) :
        MAELYS_MCP_ERR_STATE;
    if (!relay) {
        error = maelys_mcp_strdup("this call cannot open a nested client request");
    }
    long long finished_ms = realtime_milliseconds();
    if (started_ms >= 0 && finished_ms >= started_ms) {
        shift_deadline(deadline, finished_ms - started_ms);
    }
    pthread_mutex_lock(&process->state_mutex);
    char protocol[sizeof(process->negotiated_protocol)];
    (void)snprintf(protocol, sizeof(protocol), "%s", process->negotiated_protocol);
    pthread_mutex_unlock(&process->state_mutex);
    json_t *reply = nested_reply_message(protocol,
        json_string_value(nested_id), status, result, error);
    free(error);
    if (result) json_decref(result);
    if (!reply) {
        pthread_mutex_lock(&process->state_mutex);
        set_process_failure_locked(process, MAELYS_MCP_ERR_MEMORY,
            "cannot build the nested reply");
        pthread_mutex_unlock(&process->state_mutex);
        return;
    }
    maelys_mcp_result_t written = maelys_mcp_write_json_line(process->fd, reply);
    json_decref(reply);
    if (written != MAELYS_MCP_OK) {
        pthread_mutex_lock(&process->state_mutex);
        set_process_failure_locked(process, written,
            "cannot write the nested reply to provider");
        pthread_mutex_unlock(&process->state_mutex);
    }
}

static int response_deadline(unsigned int timeout_ms, struct timespec *out) {
    if (clock_gettime(CLOCK_REALTIME, out) != 0) return 0;
    out->tv_sec += (time_t)(timeout_ms / 1000u);
    long nanoseconds = out->tv_nsec +
        (long)(timeout_ms % 1000u) * 1000000L;
    out->tv_sec += nanoseconds / 1000000000L;
    out->tv_nsec = nanoseconds % 1000000000L;
    return 1;
}

static maelys_mcp_result_t process_exchange(
    maelys_mcp_process_context_t *process,
    const char *method,
    unsigned int timeout_ms,
    json_t *params,
    maelys_mcp_progress_reporter_t *reporter,
    maelys_mcp_nested_relay_t *relay,
    json_t **out_result,
    char **out_error) {
    if (!process || process->fd < 0 || !method || !out_result) return MAELYS_MCP_ERR_ARGUMENT;
    *out_result = NULL;
    pthread_mutex_lock(&process->exchange_mutex);
    if (configure_send_timeout(process->fd, timeout_ms) != MAELYS_MCP_OK) {
        replace_error(out_error, "cannot configure provider timeout");
        pthread_mutex_unlock(&process->exchange_mutex);
        return MAELYS_MCP_ERR_IO;
    }
    pthread_mutex_lock(&process->state_mutex);
    if (process->failed || process->closing) {
        replace_error(out_error, process->failure_message ?
            process->failure_message : "provider transport is unavailable");
        maelys_mcp_result_t failed = process->failure_status ?
            process->failure_status : MAELYS_MCP_ERR_PROVIDER;
        pthread_mutex_unlock(&process->state_mutex);
        pthread_mutex_unlock(&process->exchange_mutex);
        return failed;
    }
    unsigned long long id = ++process->next_id;
    process->expected_id = id;
    process->waiting = 1;
    pthread_mutex_unlock(&process->state_mutex);
    json_t *request = json_object();
    if (!request) {
        pthread_mutex_lock(&process->state_mutex);
        process->waiting = 0;
        pthread_mutex_unlock(&process->state_mutex);
        pthread_mutex_unlock(&process->exchange_mutex);
        return MAELYS_MCP_ERR_MEMORY;
    }
    pthread_mutex_lock(&process->state_mutex);
    char outbound_protocol[sizeof(process->negotiated_protocol)];
    (void)snprintf(outbound_protocol, sizeof(outbound_protocol), "%s",
        process->negotiated_protocol);
    pthread_mutex_unlock(&process->state_mutex);
    if (json_object_set_new(request, "protocol", json_string(outbound_protocol)) != 0 ||
        json_object_set_new(request, "id", json_integer((json_int_t)id)) != 0 ||
        json_object_set_new(request, "method", json_string(method)) != 0 ||
        json_object_set_new(request, "params", params ? json_incref(params) : json_object()) != 0) {
        json_decref(request);
        pthread_mutex_lock(&process->state_mutex);
        process->waiting = 0;
        pthread_mutex_unlock(&process->state_mutex);
        pthread_mutex_unlock(&process->exchange_mutex);
        return MAELYS_MCP_ERR_MEMORY;
    }
    maelys_mcp_result_t status = maelys_mcp_write_json_line(process->fd, request);
    json_decref(request);
    if (status != MAELYS_MCP_OK) {
        replace_error(out_error, "cannot write to provider");
        pthread_mutex_lock(&process->state_mutex);
        process->waiting = 0;
        set_process_failure_locked(process, status, "cannot write to provider");
        pthread_mutex_unlock(&process->state_mutex);
        pthread_mutex_unlock(&process->exchange_mutex);
        return status;
    }

    struct timespec deadline;
    if (!response_deadline(timeout_ms, &deadline)) {
        pthread_mutex_lock(&process->state_mutex);
        process->waiting = 0;
        set_process_failure_locked(process, MAELYS_MCP_ERR_IO,
            "cannot compute provider deadline");
        pthread_mutex_unlock(&process->state_mutex);
        pthread_mutex_unlock(&process->exchange_mutex);
        return MAELYS_MCP_ERR_IO;
    }
    pthread_mutex_lock(&process->state_mutex);
    while (!process->pending_response && !process->failed) {
        /* Deliver whatever the reader queued, on this thread and outside the
         * lock: emit can block on outbox admission, and the reporter is only
         * ours to use. */
        if (process->pending_progress) {
            json_t *frames = process->pending_progress;
            process->pending_progress = NULL;
            pthread_mutex_unlock(&process->state_mutex);
            emit_pending_progress(reporter, frames);
            json_decref(frames);
            pthread_mutex_lock(&process->state_mutex);
            continue;
        }
        /*
         * Same discipline as progress, for the same reason: the reader took
         * the frame off the wire, but only this thread may act on it - it owns
         * the sink, and it is the one that must block. `nested_inflight` is
         * what makes a second nested request while this one is outstanding
         * visible to the reader as the protocol violation it is.
         */
        if (process->pending_nested) {
            json_t *nested = process->pending_nested;
            process->pending_nested = NULL;
            process->nested_inflight = 1;
            pthread_mutex_unlock(&process->state_mutex);
            relay_nested_request(process, relay, nested, &deadline);
            json_decref(nested);
            pthread_mutex_lock(&process->state_mutex);
            process->nested_inflight = 0;
            continue;
        }
        int waited = pthread_cond_timedwait(
            &process->response_ready, &process->state_mutex, &deadline);
        if (waited == ETIMEDOUT) {
            set_process_failure_locked(process, MAELYS_MCP_ERR_IO,
                "provider response deadline exceeded");
            process->closing = 1;
            break;
        }
        if (waited != 0) {
            set_process_failure_locked(process, MAELYS_MCP_ERR_IO,
                "cannot wait for provider response");
            process->closing = 1;
            break;
        }
    }
    json_t *response = process->pending_response;
    process->pending_response = NULL;
    process->waiting = 0;
    /*
     * Take whatever progress is still queued along with the response. The loop
     * above exits the moment pending_response is set, so a provider that wrote
     * its progress and its result before this thread woke even once would
     * otherwise leave those frames undelivered - and the client would see the
     * response first, which is exactly the ordering the queue exists to
     * prevent. Emitted below, still ahead of the response: this function only
     * returns the result, and completing it happens further up the stack.
     */
    json_t *trailing_progress = process->pending_progress;
    process->pending_progress = NULL;
    maelys_mcp_result_t failure_status = process->failure_status;
    char *failure_message = maelys_mcp_strdup(process->failure_message);
    int closing = process->closing;
    pthread_mutex_unlock(&process->state_mutex);
    if (trailing_progress) {
        emit_pending_progress(reporter, trailing_progress);
        json_decref(trailing_progress);
    }
    if (closing) (void)shutdown(process->fd, SHUT_RDWR);
    if (!response) {
        replace_error(out_error, failure_message ? failure_message :
            "provider transport failed");
        free(failure_message);
        pthread_mutex_unlock(&process->exchange_mutex);
        return failure_status ? failure_status : MAELYS_MCP_ERR_PROVIDER;
    }
    free(failure_message);
    json_t *response_id = json_object_get(response, "id");
    json_t *protocol = json_object_get(response, "protocol");
    if (!json_is_integer(response_id) || (unsigned long long)json_integer_value(response_id) != id ||
        !supported_provider_protocol(protocol)) {
        json_decref(response);
        replace_error(out_error, "provider returned an invalid response envelope");
        pthread_mutex_unlock(&process->exchange_mutex);
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    /* The version this provider declared was already learned by the reader,
     * which raises it from every frame rather than only from responses -
     * a /5 provider announces itself on its nested requests too. */
    json_t *error = json_object_get(response, "error");
    json_t *result = json_object_get(response, "result");
    if (!!error == !!result || (error && !json_is_object(error))) {
        json_decref(response);
        replace_error(out_error, "provider response must contain exactly one result or error");
        pthread_mutex_unlock(&process->exchange_mutex);
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    if (error) {
        json_t *message = json_object_get(error, "message");
        json_t *code = json_object_get(error, "code");
        replace_error(out_error, json_is_string(message) &&
            !maelys_mcp_json_string_has_nul(message) ?
            json_string_value(message) : "provider call failed");
        int not_found = maelys_mcp_json_string_equals(code, "not_found");
        json_decref(response);
        pthread_mutex_unlock(&process->exchange_mutex);
        return not_found ? MAELYS_MCP_ERR_NOT_FOUND : MAELYS_MCP_ERR_PROVIDER;
    }
    *out_result = json_deep_copy(result);
    json_decref(response);
    pthread_mutex_unlock(&process->exchange_mutex);
    return *out_result ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static maelys_mcp_result_t process_call_relayed(
    maelys_mcp_process_context_t *process,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_nested_relay_t *relay,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    json_t *params = json_object();
    if (!params) return MAELYS_MCP_ERR_MEMORY;
    if (json_object_set_new(params, "name", json_string(request->tool_name)) != 0 ||
        json_object_set(params, "arguments", request->arguments) != 0 ||
        (request->input_responses &&
            json_object_set(params, "inputResponses", request->input_responses) != 0) ||
        (request->request_state &&
            json_object_set(params, "requestState", request->request_state) != 0) ||
        /*
         * Deep-copied rather than referenced: on a legacy channel this object
         * belongs to the channel and is shared by every call on it, and two
         * concurrent calls taking a reference to it would be two threads
         * writing one refcount. Copying reads it and touches nothing.
         */
        (request->client_capabilities &&
            json_object_set_new(params, "clientCapabilities",
                json_deep_copy(request->client_capabilities)) != 0)) {
        json_decref(params);
        return MAELYS_MCP_ERR_MEMORY;
    }
    json_t *wire_result = NULL;
    maelys_mcp_result_t result = process_exchange(
        process, "provider/call", process->call_timeout_ms, params,
        request->progress, relay, &wire_result, out_error);
    json_decref(params);
    if (result != MAELYS_MCP_OK) return result;
    json_t *type = json_is_object(wire_result) ?
        json_object_get(wire_result, "resultType") : NULL;
    if (!json_is_string(type)) {
        json_decref(wire_result);
        replace_error(out_error, "provider resultType is missing");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    if (maelys_mcp_json_string_equals(type, "complete")) {
        out_result->type = MAELYS_MCP_PROVIDER_RESULT_COMPLETE;
    } else if (maelys_mcp_json_string_equals(type, "input_required")) {
        out_result->type = MAELYS_MCP_PROVIDER_RESULT_INPUT_REQUIRED;
    } else {
        json_decref(wire_result);
        replace_error(out_error, "provider resultType is unsupported");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    json_t *content = json_object_get(wire_result, "content");
    json_t *structured = json_object_get(wire_result, "structuredContent");
    json_t *input_requests = json_object_get(wire_result, "inputRequests");
    json_t *request_state = json_object_get(wire_result, "requestState");
    json_t *is_error = json_object_get(wire_result, "isError");
    if ((content && !json_is_array(content)) ||
        (input_requests && !json_is_object(input_requests)) ||
        (request_state && (!json_is_string(request_state) ||
            maelys_mcp_json_string_has_nul(request_state))) ||
        (is_error && !json_is_boolean(is_error))) {
        json_decref(wire_result);
        replace_error(out_error, "provider result fields have invalid types");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    out_result->content = content ? json_deep_copy(content) : NULL;
    out_result->structured_content = structured ? json_deep_copy(structured) : NULL;
    out_result->input_requests = input_requests ? json_deep_copy(input_requests) : NULL;
    out_result->request_state = request_state ? json_deep_copy(request_state) : NULL;
    out_result->is_error = json_is_true(is_error);
    if ((content && !out_result->content) || (structured && !out_result->structured_content) ||
        (input_requests && !out_result->input_requests) ||
        (request_state && !out_result->request_state)) {
        maelys_mcp_provider_result_clear(out_result);
        json_decref(wire_result);
        return MAELYS_MCP_ERR_MEMORY;
    }
    json_decref(wire_result);
    return result;
}

/*
 * The two entry points differ only in whether a relay reaches process_exchange
 * - and in binding this process to the wait, which is how a provider that dies
 * mid-relay cancels the thread blocked on the client.
 */
static maelys_mcp_result_t process_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    return process_call_relayed(context, request, NULL, out_result, out_error);
}

static maelys_mcp_result_t process_call_nested(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_nested_relay_t *relay,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    if (relay) {
        relay->waiter_context = context;
        relay->waiter_bind = process_bind_nested_wait;
    }
    return process_call_relayed(context, request, relay, out_result, out_error);
}

static maelys_mcp_result_t process_read_resource_relayed(
    maelys_mcp_process_context_t *process,
    const maelys_mcp_resource_request_t *request,
    maelys_mcp_nested_relay_t *relay,
    maelys_mcp_resource_result_t *out_result,
    char **out_error) {
    json_t *params = json_object();
    if (!params) return MAELYS_MCP_ERR_MEMORY;
    if (json_object_set_new(params, "uri", json_string(request->uri)) != 0 ||
        (request->input_responses && json_object_set(params,
            "inputResponses", request->input_responses) != 0) ||
        (request->request_state && json_object_set(params,
            "requestState", request->request_state) != 0) ||
        /* Deep-copied for the same reason as in process_call_relayed. */
        (request->client_capabilities && json_object_set_new(params,
            "clientCapabilities",
            json_deep_copy(request->client_capabilities)) != 0)) {
        json_decref(params);
        return MAELYS_MCP_ERR_MEMORY;
    }
    json_t *wire = NULL;
    maelys_mcp_result_t status = process_exchange(process, "provider/readResource",
        process->call_timeout_ms, params, NULL, relay, &wire, out_error);
    json_decref(params);
    if (status != MAELYS_MCP_OK) return status;
    json_t *type = json_is_object(wire) ? json_object_get(wire, "resultType") : NULL;
    if (maelys_mcp_json_string_equals(type, "complete")) {
        out_result->type = MAELYS_MCP_RESOURCE_RESULT_COMPLETE;
        json_t *contents = json_object_get(wire, "contents");
        if (!json_is_array(contents)) status = MAELYS_MCP_ERR_PROTOCOL;
        else out_result->contents = json_deep_copy(contents);
    } else if (maelys_mcp_json_string_equals(type, "input_required")) {
        out_result->type = MAELYS_MCP_RESOURCE_RESULT_INPUT_REQUIRED;
        json_t *requests = json_object_get(wire, "inputRequests");
        json_t *state = json_object_get(wire, "requestState");
        if ((requests && !json_is_object(requests)) ||
            (state && (!json_is_string(state) || maelys_mcp_json_string_has_nul(state)))) {
            status = MAELYS_MCP_ERR_PROTOCOL;
        } else {
            out_result->input_requests = requests ? json_deep_copy(requests) : NULL;
            out_result->request_state = state ? json_deep_copy(state) : NULL;
        }
    } else {
        status = MAELYS_MCP_ERR_PROTOCOL;
    }
    if (status == MAELYS_MCP_OK &&
        ((out_result->type == MAELYS_MCP_RESOURCE_RESULT_COMPLETE && !out_result->contents) ||
         (json_object_get(wire, "inputRequests") && !out_result->input_requests) ||
         (json_object_get(wire, "requestState") && !out_result->request_state))) {
        status = MAELYS_MCP_ERR_MEMORY;
    }
    if (status != MAELYS_MCP_OK) {
        maelys_mcp_resource_result_clear(out_result);
        replace_error(out_error, "provider returned an invalid resource result");
    }
    json_decref(wire);
    return status;
}

static maelys_mcp_result_t process_read_resource(
    void *context,
    const maelys_mcp_resource_request_t *request,
    maelys_mcp_resource_result_t *out_result,
    char **out_error) {
    return process_read_resource_relayed(context, request, NULL, out_result,
        out_error);
}

static maelys_mcp_result_t process_read_resource_nested(
    void *context,
    const maelys_mcp_resource_request_t *request,
    maelys_mcp_nested_relay_t *relay,
    maelys_mcp_resource_result_t *out_result,
    char **out_error) {
    if (relay) {
        relay->waiter_context = context;
        relay->waiter_bind = process_bind_nested_wait;
    }
    return process_read_resource_relayed(context, request, relay, out_result,
        out_error);
}

static maelys_mcp_result_t process_activate(void *context, char **out_error) {
    maelys_mcp_process_context_t *process = context;
    pthread_mutex_lock(&process->state_mutex);
    process->activation_pending = 1;
    pthread_mutex_unlock(&process->state_mutex);
    json_t *result = NULL;
    maelys_mcp_result_t status = process_exchange(process, "provider/activate",
        process->describe_timeout_ms, NULL, NULL, NULL, &result, out_error);
    if (status == MAELYS_MCP_OK && !json_is_object(result)) {
        replace_error(out_error, "provider activation result must be an object");
        status = MAELYS_MCP_ERR_PROTOCOL;
    }
    pthread_mutex_lock(&process->state_mutex);
    process->events_enabled = status == MAELYS_MCP_OK;
    process->activation_pending = 0;
    pthread_cond_broadcast(&process->response_ready);
    pthread_mutex_unlock(&process->state_mutex);
    if (result) json_decref(result);
    return status;
}

static void process_destroy(void *context) {
    maelys_mcp_process_context_t *process = context;
    if (!process) return;
    if (process->fd >= 0) {
        json_t *ignored = NULL;
        char *error = NULL;
        (void)process_exchange(process, "provider/shutdown",
            process->shutdown_timeout_ms, NULL, NULL, NULL, &ignored, &error);
        if (ignored) json_decref(ignored);
        free(error);
        pthread_mutex_lock(&process->state_mutex);
        process->closing = 1;
        pthread_mutex_unlock(&process->state_mutex);
        (void)shutdown(process->fd, SHUT_RDWR);
        if (process->reader_started) {
            (void)pthread_join(process->reader_thread, NULL);
            process->reader_started = 0;
        }
        close(process->fd);
        process->fd = -1;
    }
    /*
     * The protocol goodbye above stays here, above the seam: a
     * provider/shutdown exchange is a protocol fact and belongs to this
     * provider kind. Everything after it is the one bounded ladder, shared
     * with the proxy.
     *
     * The ladder can report a containment failure - a child that outlived a
     * forced stop - and this is where that report stops, because the destroy
     * contract is void from here all the way up through
     * maelys_mcp_provider_destroy and there is no caller left to hand it to.
     * The ladder's own return is what tests/test_process_launcher.c pins, at
     * the level where a caller does exist. What matters here is that teardown
     * still returns: bounded at every rung, including the last.
     */
    char *containment_error = NULL;
    (void)maelys_mcp_process_shutdown(process->launcher, &process->instance,
        process->shutdown_timeout_ms, process->shutdown_timeout_ms,
        &containment_error);
    free(containment_error);
    if (process->reader) {
        maelys_mcp_line_reader_clear(process->reader);
        free(process->reader);
    }
    if (process->pending_response) json_decref(process->pending_response);
    if (process->pending_progress) json_decref(process->pending_progress);
    /* A nested request the reader took off the wire and no call ever collected
     * - which is exactly what a provider that died or broke the one-at-a-time
     * rule leaves behind, so it is the normal case, not the exotic one. */
    if (process->pending_nested) json_decref(process->pending_nested);
    free(process->failure_message);
    if (process->response_ready_initialized) {
        pthread_cond_destroy(&process->response_ready);
    }
    if (process->state_mutex_initialized) {
        pthread_mutex_destroy(&process->state_mutex);
    }
    if (process->exchange_mutex_initialized) {
        pthread_mutex_destroy(&process->exchange_mutex);
    }
    free(process);
}

/*
 * The launch itself happens behind the seam: this compiles a spec, hands it to
 * the launcher it was given, and never sees a pid, a socketpair or a fork.
 * docs/launch-contract-design.md explains why that matters more than the
 * duplication it removes - a second fork/exec site is a second bypass of any
 * confinement the launcher applies.
 *
 * `extra_args` is manifest v2's "args" (docs/manifest.md): a NULL-terminated
 * vector of EXTRA arguments only, NULL for the v1 callers that carry none.
 * `execution_profile` is manifest v2's "executionProfile", opaque pass-through
 * to the launcher; NULL for those same callers.
 */
static maelys_mcp_result_t spawn_process(
    const maelys_mcp_provider_process_options_t *options,
    char *const *extra_args,
    const char *execution_profile,
    const maelys_mcp_process_launcher_t *launcher,
    maelys_mcp_process_context_t **out_process,
    char **out_error) {
    /*
     * The COMPLETE vector, argv[0] included: nothing below the seam ever
     * receives "extra arguments" - --provider is what puts a maelys-provider
     * binary into provider mode, and the runtime compiles it rather than
     * letting configuration write argv[1], which would turn a provider
     * declaration into an arbitrary invocation of a trusted binary. Manifest
     * v2's "args" (docs/launch-contract-design.md, "Two layers of argv") is
     * appended after it, never in front of it.
     */
    size_t extra_count = 0u;
    if (extra_args) {
        while (extra_args[extra_count]) ++extra_count;
    }
    char **argv = calloc(extra_count + 3u, sizeof(*argv));
    if (!argv) return MAELYS_MCP_ERR_MEMORY;
    argv[0] = (char *)options->executable_path;
    argv[1] = (char *)"--provider";
    for (size_t index = 0; index < extra_count; ++index) {
        argv[2u + index] = extra_args[index];
    }
    argv[2u + extra_count] = NULL;
    maelys_mcp_process_spec_t spec = {
        .executable_path = options->executable_path,
        .argv = argv,
        .execution_profile = execution_profile,
        .max_message_bytes = options->max_message_bytes,
        /* The enclosing budget this launch has to complete inside - a provider
         * that is not up in time to answer describe is not up. Carried rather
         * than enforced: this runtime has no thread to time a spawn out with,
         * and a launcher that can block is the one that has to honour it. */
        .spawn_timeout_ms = options->describe_timeout_ms,
        .grace_timeout_ms = options->shutdown_timeout_ms,
        .force_timeout_ms = options->shutdown_timeout_ms,
        /*
         * Derived from the child's protocol type, never configured. Native
         * children run a first-party SDK, so MAELYS_MCP_PROCESS_FD_ISOLATED
         * is implemented and available to this kind - unlike mcp-proxy
         * upstreams, which stay STDIO structurally. But this call site does
         * not select it yet: the transition plan's first step keeps STDIO the
         * native default through M4, until both first-party SDKs have shipped
         * MAELYS_PROVIDER_FD support and a deprecation window has passed
         * (docs/launch-contract-design.md, "The layout is a function of the
         * child's protocol type"). Selection this release is via the public
         * spec only - an embedder driving the launcher directly, as the
         * conformance suite does, may opt in today.
         */
        .fd_layout = MAELYS_MCP_PROCESS_FD_STDIO
    };
    maelys_mcp_process_instance_t instance = {.protocol_fd = -1};
    maelys_mcp_result_t status = maelys_mcp_process_launch(launcher, &spec,
        &instance, out_error);
    /* Every spec field is read before spawn returns (process_launcher.h); the
     * vector this compiled has no life beyond that call. */
    free(argv);
    if (status != MAELYS_MCP_OK) return status;
    int fd = instance.protocol_fd;
    maelys_mcp_process_context_t *process = calloc(1u, sizeof(*process));
    if (!process) {
        close(fd);
        maelys_mcp_process_abandon(launcher, &instance,
            options->shutdown_timeout_ms);
        return MAELYS_MCP_ERR_MEMORY;
    }
    process->reader = calloc(1u, sizeof(*process->reader));
    if (!process->reader || maelys_mcp_line_reader_init(
        process->reader, options->max_message_bytes) != MAELYS_MCP_OK) {
        free(process->reader);
        free(process);
        close(fd);
        maelys_mcp_process_abandon(launcher, &instance,
            options->shutdown_timeout_ms);
        return MAELYS_MCP_ERR_MEMORY;
    }
    if (pthread_mutex_init(&process->exchange_mutex, NULL) != 0) {
        maelys_mcp_line_reader_clear(process->reader);
        free(process->reader);
        free(process);
        close(fd);
        maelys_mcp_process_abandon(launcher, &instance,
            options->shutdown_timeout_ms);
        return MAELYS_MCP_ERR_IO;
    }
    process->exchange_mutex_initialized = 1;
    if (pthread_mutex_init(&process->state_mutex, NULL) != 0) {
        pthread_mutex_destroy(&process->exchange_mutex);
        maelys_mcp_line_reader_clear(process->reader);
        free(process->reader);
        free(process);
        close(fd);
        maelys_mcp_process_abandon(launcher, &instance,
            options->shutdown_timeout_ms);
        return MAELYS_MCP_ERR_IO;
    }
    process->state_mutex_initialized = 1;
    if (pthread_cond_init(&process->response_ready, NULL) != 0) {
        pthread_mutex_destroy(&process->state_mutex);
        pthread_mutex_destroy(&process->exchange_mutex);
        maelys_mcp_line_reader_clear(process->reader);
        free(process->reader);
        free(process);
        close(fd);
        maelys_mcp_process_abandon(launcher, &instance,
            options->shutdown_timeout_ms);
        return MAELYS_MCP_ERR_IO;
    }
    process->response_ready_initialized = 1;
    process->launcher = launcher;
    process->instance = instance;
    process->fd = fd;
    process->max_message_bytes = options->max_message_bytes;
    /* Open at the floor: until this provider tells us otherwise, assume it
     * only understands the version every provider understands. */
    (void)snprintf(process->negotiated_protocol,
        sizeof(process->negotiated_protocol), "%s",
        MAELYS_MCP_PROVIDER_PROTOCOL_FLOOR);
    process->describe_timeout_ms = options->describe_timeout_ms;
    process->call_timeout_ms = options->call_timeout_ms;
    process->shutdown_timeout_ms = options->shutdown_timeout_ms;
    if (pthread_create(&process->reader_thread, NULL,
        process_reader_main, process) != 0) {
        pthread_cond_destroy(&process->response_ready);
        pthread_mutex_destroy(&process->state_mutex);
        pthread_mutex_destroy(&process->exchange_mutex);
        maelys_mcp_line_reader_clear(process->reader);
        free(process->reader);
        free(process);
        close(fd);
        maelys_mcp_process_abandon(launcher, &instance,
            options->shutdown_timeout_ms);
        return MAELYS_MCP_ERR_IO;
    }
    process->reader_started = 1;
    *out_process = process;
    return MAELYS_MCP_OK;
}

maelys_mcp_result_t maelys_mcp_provider_spawn(
    const char *executable_path,
    size_t max_message_bytes,
    maelys_mcp_provider_t **out_provider,
    char **out_error) {
    maelys_mcp_provider_process_options_t options = {
        .executable_path = executable_path,
        .max_message_bytes = max_message_bytes,
        .describe_timeout_ms = MAELYS_MCP_DEFAULT_PROVIDER_DESCRIBE_TIMEOUT_MS,
        .call_timeout_ms = MAELYS_MCP_DEFAULT_PROVIDER_CALL_TIMEOUT_MS,
        .shutdown_timeout_ms = MAELYS_MCP_DEFAULT_PROVIDER_SHUTDOWN_TIMEOUT_MS
    };
    return maelys_mcp_provider_spawn_with_options(&options, out_provider, out_error);
}

static maelys_mcp_result_t parse_resources(
    json_t *array,
    maelys_mcp_resource_t **out_resources,
    size_t *out_count) {
    *out_resources = NULL;
    *out_count = 0;
    if (!array) return MAELYS_MCP_OK;
    if (!json_is_array(array)) return MAELYS_MCP_ERR_PROTOCOL;
    size_t count = json_array_size(array);
    maelys_mcp_resource_t *resources = count ? calloc(count, sizeof(*resources)) : NULL;
    if (count && !resources) return MAELYS_MCP_ERR_MEMORY;
    for (size_t index = 0; index < count; ++index) {
        json_t *value = json_array_get(array, index);
        json_t *uri = json_is_object(value) ? json_object_get(value, "uri") : NULL;
        json_t *name = json_is_object(value) ? json_object_get(value, "name") : NULL;
        json_t *title = json_is_object(value) ? json_object_get(value, "title") : NULL;
        json_t *description = json_is_object(value) ? json_object_get(value, "description") : NULL;
        json_t *mime = json_is_object(value) ? json_object_get(value, "mimeType") : NULL;
        json_t *size = json_is_object(value) ? json_object_get(value, "size") : NULL;
        if (!json_is_string(uri) || maelys_mcp_json_string_has_nul(uri) ||
            !json_is_string(name) || maelys_mcp_json_string_has_nul(name) ||
            (title && (!json_is_string(title) || maelys_mcp_json_string_has_nul(title))) ||
            (description && (!json_is_string(description) ||
                maelys_mcp_json_string_has_nul(description))) ||
            (mime && (!json_is_string(mime) || maelys_mcp_json_string_has_nul(mime))) ||
            (size && (!json_is_integer(size) || json_integer_value(size) < 0))) {
            free(resources);
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        resources[index] = (maelys_mcp_resource_t){
            .uri = json_string_value(uri), .name = json_string_value(name),
            .title = title ? json_string_value(title) : NULL,
            .description = description ? json_string_value(description) : NULL,
            .mime_type = mime ? json_string_value(mime) : NULL,
            .has_size = size != NULL,
            .size = size ? (long long)json_integer_value(size) : 0
        };
    }
    *out_resources = resources;
    *out_count = count;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t parse_resource_templates(
    json_t *array,
    maelys_mcp_resource_template_t **out_templates,
    size_t *out_count) {
    *out_templates = NULL;
    *out_count = 0;
    if (!array) return MAELYS_MCP_OK;
    if (!json_is_array(array)) return MAELYS_MCP_ERR_PROTOCOL;
    size_t count = json_array_size(array);
    maelys_mcp_resource_template_t *templates = count ? calloc(count, sizeof(*templates)) : NULL;
    if (count && !templates) return MAELYS_MCP_ERR_MEMORY;
    for (size_t index = 0; index < count; ++index) {
        json_t *value = json_array_get(array, index);
        json_t *uri = json_is_object(value) ? json_object_get(value, "uriTemplate") : NULL;
        json_t *name = json_is_object(value) ? json_object_get(value, "name") : NULL;
        json_t *title = json_is_object(value) ? json_object_get(value, "title") : NULL;
        json_t *description = json_is_object(value) ? json_object_get(value, "description") : NULL;
        json_t *mime = json_is_object(value) ? json_object_get(value, "mimeType") : NULL;
        if (!json_is_string(uri) || maelys_mcp_json_string_has_nul(uri) ||
            !json_is_string(name) || maelys_mcp_json_string_has_nul(name) ||
            (title && (!json_is_string(title) || maelys_mcp_json_string_has_nul(title))) ||
            (description && (!json_is_string(description) ||
                maelys_mcp_json_string_has_nul(description))) ||
            (mime && (!json_is_string(mime) || maelys_mcp_json_string_has_nul(mime)))) {
            free(templates);
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        templates[index] = (maelys_mcp_resource_template_t){
            .uri_template = json_string_value(uri), .name = json_string_value(name),
            .title = title ? json_string_value(title) : NULL,
            .description = description ? json_string_value(description) : NULL,
            .mime_type = mime ? json_string_value(mime) : NULL
        };
    }
    *out_templates = templates;
    *out_count = count;
    return MAELYS_MCP_OK;
}

maelys_mcp_result_t maelys_mcp_provider_spawn_with_options(
    const maelys_mcp_provider_process_options_t *options,
    maelys_mcp_provider_t **out_provider,
    char **out_error) {
    /* The API that predates the seam, unchanged: it binds the launcher that
     * reproduces what it always did. */
    return maelys_mcp_provider_spawn_with_launcher(options,
        maelys_mcp_posix_launcher(), out_provider, out_error);
}

/*
 * The shared body behind every native spawn entry point. `extra_args` and
 * `execution_profile` are manifest v2's "args" and "executionProfile"
 * (docs/manifest.md); the pre-v2 entry points pass NULL for both, which
 * reproduces their exact pre-v2 behaviour through spawn_process.
 */
static maelys_mcp_result_t provider_spawn_with_launcher_and_args(
    const maelys_mcp_provider_process_options_t *options,
    char *const *extra_args,
    const char *execution_profile,
    const maelys_mcp_process_launcher_t *launcher,
    maelys_mcp_provider_t **out_provider,
    char **out_error) {
    if (!launcher) {
        replace_error(out_error, "provider launcher must not be null");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    if (!options || !options->executable_path || options->executable_path[0] != '/' ||
        !out_provider) {
        replace_error(out_error, "provider executable must be an absolute path");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    maelys_mcp_provider_process_options_t normalized = *options;
    if (!normalized.max_message_bytes) {
        normalized.max_message_bytes = MAELYS_MCP_DEFAULT_MAX_MESSAGE_BYTES;
    }
    if (!normalized.describe_timeout_ms) {
        normalized.describe_timeout_ms = MAELYS_MCP_DEFAULT_PROVIDER_DESCRIBE_TIMEOUT_MS;
    }
    if (!normalized.call_timeout_ms) {
        normalized.call_timeout_ms = MAELYS_MCP_DEFAULT_PROVIDER_CALL_TIMEOUT_MS;
    }
    if (!normalized.shutdown_timeout_ms) {
        normalized.shutdown_timeout_ms = MAELYS_MCP_DEFAULT_PROVIDER_SHUTDOWN_TIMEOUT_MS;
    }
    *out_provider = NULL;
    maelys_mcp_process_context_t *process = NULL;
    maelys_mcp_result_t status = spawn_process(
        &normalized, extra_args, execution_profile, launcher, &process, out_error);
    if (status != MAELYS_MCP_OK) return status;

    json_t *description = NULL;
    status = process_exchange(process, "provider/describe",
        process->describe_timeout_ms, NULL, NULL, NULL, &description, out_error);
    if (status != MAELYS_MCP_OK) {
        process_destroy(process);
        return status;
    }
    json_t *name = json_object_get(description, "name");
    json_t *version = json_object_get(description, "version");
    json_t *tools_json = json_object_get(description, "tools");
    json_t *resources_json = json_object_get(description, "resources");
    json_t *templates_json = json_object_get(description, "resourceTemplates");
    if (!json_is_string(name) || maelys_mcp_json_string_has_nul(name) ||
        !json_is_string(version) || maelys_mcp_json_string_has_nul(version) ||
        !json_is_array(tools_json)) {
        json_decref(description);
        process_destroy(process);
        replace_error(out_error, "provider description is invalid");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    size_t count = json_array_size(tools_json);
    maelys_mcp_tool_t *tools = count ? calloc(count, sizeof(*tools)) : NULL;
    if (count && !tools) {
        json_decref(description);
        process_destroy(process);
        return MAELYS_MCP_ERR_MEMORY;
    }
    for (size_t index = 0; index < count; ++index) {
        json_t *tool = json_array_get(tools_json, index);
        json_t *tool_name = json_object_get(tool, "name");
        json_t *title = json_object_get(tool, "title");
        json_t *tool_description = json_object_get(tool, "description");
        json_t *input_schema = json_object_get(tool, "inputSchema");
        json_t *output_schema = json_object_get(tool, "outputSchema");
        json_t *effect = json_object_get(tool, "effect");
        maelys_mcp_tool_effect_t parsed_effect;
        if (!json_is_string(tool_name) || maelys_mcp_json_string_has_nul(tool_name) ||
            !json_is_string(tool_description) || maelys_mcp_json_string_has_nul(tool_description) ||
            (title && (!json_is_string(title) || maelys_mcp_json_string_has_nul(title))) ||
            !json_is_object(input_schema) || (output_schema && !json_is_object(output_schema)) ||
            !json_is_string(effect) || maelys_mcp_json_string_has_nul(effect) ||
            maelys_mcp_tool_effect_parse(
                json_string_value(effect), &parsed_effect) != MAELYS_MCP_OK) {
            free(tools);
            json_decref(description);
            process_destroy(process);
            replace_error(out_error, "provider tool description is invalid");
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        char *schema_error = NULL;
        if (maelys_mcp_validate_schema_definition(input_schema, 1, &schema_error) != MAELYS_MCP_OK ||
            (output_schema &&
             maelys_mcp_validate_schema_definition(output_schema, 0, &schema_error) != MAELYS_MCP_OK)) {
            free(tools);
            json_decref(description);
            process_destroy(process);
            replace_error(out_error, schema_error ? schema_error : "provider schema is unsupported");
            free(schema_error);
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        free(schema_error);
        tools[index] = (maelys_mcp_tool_t){
            .name = json_string_value(tool_name),
            .title = json_is_string(title) ? json_string_value(title) : json_string_value(tool_name),
            .description = json_string_value(tool_description),
            .input_schema = input_schema,
            .output_schema = output_schema,
            .effect = parsed_effect
        };
    }
    maelys_mcp_resource_t *resources = NULL;
    size_t resource_count = 0;
    maelys_mcp_resource_template_t *templates = NULL;
    size_t template_count = 0;
    status = parse_resources(resources_json, &resources, &resource_count);
    if (status == MAELYS_MCP_OK) {
        status = parse_resource_templates(templates_json, &templates, &template_count);
    }
    if (status != MAELYS_MCP_OK) {
        free(resources);
        free(templates);
        free(tools);
        json_decref(description);
        process_destroy(process);
        replace_error(out_error, "provider resource description is invalid");
        return status;
    }
    maelys_mcp_provider_config_t config = {
        .name = json_string_value(name),
        .version = json_string_value(version),
        .tools = tools,
        .tool_count = count,
        .resources = resources,
        .resource_count = resource_count,
        .resource_templates = templates,
        .resource_template_count = template_count,
        .call = process_call,
        .read_resource = (resource_count || template_count) ? process_read_resource : NULL,
        .destroy = process_destroy,
        .context = process
    };
    status = maelys_mcp_provider_create(&config, out_provider);
    free(resources);
    free(templates);
    free(tools);
    json_decref(description);
    if (status != MAELYS_MCP_OK) {
        process_destroy(process);
    } else {
        /*
         * Registered unconditionally, not gated on the version this provider
         * declared: the nesting-capable callback is a superset of the plain
         * one, and a provider that never sends a nested request never notices
         * the difference. Gating here would only mean a provider that
         * announces /5 late - on the frame that opens its first nested request
         * - could never be relayed.
         */
        maelys_mcp_provider_nested_handlers_t handlers = {
            .call = process_call_nested,
            .read_resource = (resource_count || template_count) ?
                process_read_resource_nested : NULL
        };
        (void)maelys_mcp_provider_set_nested_handlers(*out_provider, &handlers);
        (*out_provider)->activate = process_activate;
        (*out_provider)->activated = 0;
        pthread_mutex_lock(&process->state_mutex);
        process->owner = *out_provider;
        pthread_mutex_unlock(&process->state_mutex);
    }
    return status;
}

maelys_mcp_result_t maelys_mcp_provider_spawn_with_launcher(
    const maelys_mcp_provider_process_options_t *options,
    const maelys_mcp_process_launcher_t *launcher,
    maelys_mcp_provider_t **out_provider,
    char **out_error) {
    /* No manifest v2 data: byte-identical to what this entry point always
     * did. */
    return provider_spawn_with_launcher_and_args(
        options, NULL, NULL, launcher, out_provider, out_error);
}

maelys_mcp_result_t maelys_mcp_provider_spawn_with_args(
    const maelys_mcp_provider_process_options_t *options,
    char *const *args,
    const char *execution_profile,
    maelys_mcp_provider_t **out_provider,
    char **out_error) {
    /* Always the POSIX launcher, like maelys_mcp_provider_spawn_with_options:
     * this entry point exists to carry manifest v2 data, not to name a
     * launcher. */
    return provider_spawn_with_launcher_and_args(options, args,
        execution_profile, maelys_mcp_posix_launcher(), out_provider, out_error);
}
