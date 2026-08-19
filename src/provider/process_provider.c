#include "src/internal/internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
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

/* Accepts either version: the floor every provider speaks, or the current
 * one. Anything else is still a protocol failure. */
static int supported_provider_protocol(const json_t *protocol) {
    return maelys_mcp_json_string_equals(protocol, MAELYS_MCP_PROVIDER_PROTOCOL) ||
        maelys_mcp_json_string_equals(protocol, MAELYS_MCP_PROVIDER_PROTOCOL_FLOOR);
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
        if (!supported_provider_protocol(protocol)) {
            json_decref(message);
            pthread_mutex_lock(&process->state_mutex);
            set_process_failure_locked(process, MAELYS_MCP_ERR_PROTOCOL,
                "provider returned an invalid protocol envelope");
            pthread_mutex_unlock(&process->state_mutex);
            break;
        }
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
                    (void)json_array_append(process->pending_progress, frame);
                    pthread_cond_broadcast(&process->response_ready);
                }
            }
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
    /* The SDKs declare their own version in responses rather than echoing
     * ours, so the first response is enough to learn what this provider
     * speaks and to stop addressing it at the floor. */
    if (maelys_mcp_json_string_equals(protocol, MAELYS_MCP_PROVIDER_PROTOCOL)) {
        pthread_mutex_lock(&process->state_mutex);
        (void)snprintf(process->negotiated_protocol,
            sizeof(process->negotiated_protocol), "%s", MAELYS_MCP_PROVIDER_PROTOCOL);
        pthread_mutex_unlock(&process->state_mutex);
    }
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

static maelys_mcp_result_t process_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    maelys_mcp_process_context_t *process = context;
    json_t *params = json_object();
    if (!params) return MAELYS_MCP_ERR_MEMORY;
    if (json_object_set_new(params, "name", json_string(request->tool_name)) != 0 ||
        json_object_set(params, "arguments", request->arguments) != 0 ||
        (request->input_responses &&
            json_object_set(params, "inputResponses", request->input_responses) != 0) ||
        (request->request_state &&
            json_object_set(params, "requestState", request->request_state) != 0) ||
        (request->client_capabilities &&
            json_object_set(params, "clientCapabilities", request->client_capabilities) != 0)) {
        json_decref(params);
        return MAELYS_MCP_ERR_MEMORY;
    }
    json_t *wire_result = NULL;
    maelys_mcp_result_t result = process_exchange(
        process, "provider/call", process->call_timeout_ms, params,
        request->progress, &wire_result, out_error);
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

static maelys_mcp_result_t process_read_resource(
    void *context,
    const maelys_mcp_resource_request_t *request,
    maelys_mcp_resource_result_t *out_result,
    char **out_error) {
    maelys_mcp_process_context_t *process = context;
    json_t *params = json_object();
    if (!params) return MAELYS_MCP_ERR_MEMORY;
    if (json_object_set_new(params, "uri", json_string(request->uri)) != 0 ||
        (request->input_responses && json_object_set(params,
            "inputResponses", request->input_responses) != 0) ||
        (request->request_state && json_object_set(params,
            "requestState", request->request_state) != 0) ||
        (request->client_capabilities && json_object_set(params,
            "clientCapabilities", request->client_capabilities) != 0)) {
        json_decref(params);
        return MAELYS_MCP_ERR_MEMORY;
    }
    json_t *wire = NULL;
    maelys_mcp_result_t status = process_exchange(process, "provider/readResource",
        process->call_timeout_ms, params, NULL, &wire, out_error);
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

static long long monotonic_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (long long)now.tv_sec * 1000LL + (long long)now.tv_nsec / 1000000LL;
}

static int wait_for_child(pid_t pid, unsigned int timeout_ms) {
    long long start = monotonic_milliseconds();
    if (start < 0) return 0;
    for (;;) {
        int status = 0;
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid || (waited < 0 && errno == ECHILD)) return 1;
        if (waited < 0 && errno != EINTR) return 0;
        long long now = monotonic_milliseconds();
        if (now < 0 || now - start >= (long long)timeout_ms) return 0;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000L};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
}

static maelys_mcp_result_t process_activate(void *context, char **out_error) {
    maelys_mcp_process_context_t *process = context;
    pthread_mutex_lock(&process->state_mutex);
    process->activation_pending = 1;
    pthread_mutex_unlock(&process->state_mutex);
    json_t *result = NULL;
    maelys_mcp_result_t status = process_exchange(process, "provider/activate",
        process->describe_timeout_ms, NULL, NULL, &result, out_error);
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
            process->shutdown_timeout_ms, NULL, NULL, &ignored, &error);
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
    if (process->pid > 0 && !wait_for_child(process->pid, process->shutdown_timeout_ms)) {
        (void)kill(process->pid, SIGTERM);
        if (!wait_for_child(process->pid, process->shutdown_timeout_ms)) {
            (void)kill(process->pid, SIGKILL);
            while (waitpid(process->pid, NULL, 0) < 0 && errno == EINTR) {}
        }
    }
    if (process->reader) {
        maelys_mcp_line_reader_clear(process->reader);
        free(process->reader);
    }
    if (process->pending_response) json_decref(process->pending_response);
    if (process->pending_progress) json_decref(process->pending_progress);
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

static int set_close_on_exec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static maelys_mcp_result_t spawn_process(
    const maelys_mcp_provider_process_options_t *options,
    maelys_mcp_process_context_t **out_process,
    char **out_error) {
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        replace_error(out_error, "socketpair failed");
        return MAELYS_MCP_ERR_IO;
    }
    if (!set_close_on_exec(sockets[0]) || !set_close_on_exec(sockets[1])) {
        close(sockets[0]);
        close(sockets[1]);
        replace_error(out_error, "cannot protect provider descriptors");
        return MAELYS_MCP_ERR_IO;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(sockets[0]);
        close(sockets[1]);
        replace_error(out_error, "fork failed");
        return MAELYS_MCP_ERR_IO;
    }
    if (pid == 0) {
        close(sockets[0]);
        if (dup2(sockets[1], STDIN_FILENO) < 0 || dup2(sockets[1], STDOUT_FILENO) < 0) _exit(126);
        if (sockets[1] != STDIN_FILENO && sockets[1] != STDOUT_FILENO) close(sockets[1]);
        char *const argv[] = {(char *)options->executable_path, (char *)"--provider", NULL};
        char *const environment[] = {
#ifdef __APPLE__
            (char *)"PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin",
#else
            (char *)"PATH=/usr/local/bin:/usr/bin:/bin",
#endif
            (char *)"LANG=C",
            (char *)"LC_ALL=C",
            NULL
        };
        execve(options->executable_path, argv, environment);
        _exit(127);
    }
    close(sockets[1]);
#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    if (setsockopt(sockets[0], SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) != 0) {
        close(sockets[0]);
        (void)kill(pid, SIGTERM);
        (void)waitpid(pid, NULL, 0);
        replace_error(out_error, "cannot configure provider socket safety");
        return MAELYS_MCP_ERR_IO;
    }
#endif
    maelys_mcp_process_context_t *process = calloc(1u, sizeof(*process));
    if (!process) {
        close(sockets[0]);
        (void)kill(pid, SIGTERM);
        (void)waitpid(pid, NULL, 0);
        return MAELYS_MCP_ERR_MEMORY;
    }
    process->reader = calloc(1u, sizeof(*process->reader));
    if (!process->reader || maelys_mcp_line_reader_init(
        process->reader, options->max_message_bytes) != MAELYS_MCP_OK) {
        free(process->reader);
        free(process);
        close(sockets[0]);
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return MAELYS_MCP_ERR_MEMORY;
    }
    if (pthread_mutex_init(&process->exchange_mutex, NULL) != 0) {
        maelys_mcp_line_reader_clear(process->reader);
        free(process->reader);
        free(process);
        close(sockets[0]);
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return MAELYS_MCP_ERR_IO;
    }
    process->exchange_mutex_initialized = 1;
    if (pthread_mutex_init(&process->state_mutex, NULL) != 0) {
        pthread_mutex_destroy(&process->exchange_mutex);
        maelys_mcp_line_reader_clear(process->reader);
        free(process->reader);
        free(process);
        close(sockets[0]);
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return MAELYS_MCP_ERR_IO;
    }
    process->state_mutex_initialized = 1;
    if (pthread_cond_init(&process->response_ready, NULL) != 0) {
        pthread_mutex_destroy(&process->state_mutex);
        pthread_mutex_destroy(&process->exchange_mutex);
        maelys_mcp_line_reader_clear(process->reader);
        free(process->reader);
        free(process);
        close(sockets[0]);
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return MAELYS_MCP_ERR_IO;
    }
    process->response_ready_initialized = 1;
    process->pid = pid;
    process->fd = sockets[0];
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
        close(sockets[0]);
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
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
        &normalized, &process, out_error);
    if (status != MAELYS_MCP_OK) return status;

    json_t *description = NULL;
    status = process_exchange(process, "provider/describe", 
        process->describe_timeout_ms, NULL, NULL, &description, out_error);
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
        (*out_provider)->activate = process_activate;
        (*out_provider)->activated = 0;
        pthread_mutex_lock(&process->state_mutex);
        process->owner = *out_provider;
        pthread_mutex_unlock(&process->state_mutex);
    }
    return status;
}
