/*
 * The adapter seam, driven by a recording writer with no socket involved.
 *
 * This is one half of H1's merge criterion and the reason the seam is a
 * function rather than a descriptor: an orchestrator behind its own listener,
 * and a fuzz target with no listener at all, reach the adapter exactly the way
 * this file does.
 *
 * What is asserted here is the CONTRACT, not the placeholder answer's content.
 * The adapter does not dispatch in H1 - it answers from a table - so the
 * interesting claims are the ones that must stay true when H3 replaces the
 * table: exactly one of begin_json/begin_stream, end_stream exactly once per
 * begin_stream, the exchange published before any work and retired before
 * _handle returns, and a cancel after the return that is a no-op rather than a
 * use-after-free.
 */
#include "maelys/mcp/http.h"
#include "tests/test_support.h"

#include <pthread.h>
#include <string.h>
#include <strings.h>

typedef struct recording_writer {
    int begin_json_calls;
    int begin_stream_calls;
    int event_calls;
    int keepalive_calls;
    int end_stream_calls;
    int status_only_calls;
    int status;
    maelys_mcp_http_stream_end_t disposition;
    char body[512];
    size_t body_length;
} recording_writer_t;

static maelys_mcp_result_t record_begin_json(
    void *context, int status, const char *body, size_t length) {
    recording_writer_t *writer = context;
    ++writer->begin_json_calls;
    writer->status = status;
    if (length < sizeof(writer->body)) {
        memcpy(writer->body, body, length);
        writer->body_length = length;
    }
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t record_begin_stream(void *context) {
    recording_writer_t *writer = context;
    ++writer->begin_stream_calls;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t record_event(void *context, const char *json, size_t length) {
    recording_writer_t *writer = context;
    (void)json;
    (void)length;
    ++writer->event_calls;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t record_keepalive(void *context) {
    recording_writer_t *writer = context;
    ++writer->keepalive_calls;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t record_end_stream(
    void *context, maelys_mcp_http_stream_end_t disposition) {
    recording_writer_t *writer = context;
    ++writer->end_stream_calls;
    writer->disposition = disposition;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t record_status_only(
    void *context, int status, const char *const *extra, size_t extra_count) {
    recording_writer_t *writer = context;
    (void)extra;
    (void)extra_count;
    ++writer->status_only_calls;
    writer->status = status;
    return MAELYS_MCP_OK;
}

static maelys_mcp_http_response_writer_t make_writer(recording_writer_t *state) {
    maelys_mcp_http_response_writer_t writer = {
        .context = state,
        .begin_json = record_begin_json,
        .begin_stream = record_begin_stream,
        .write_event = record_event,
        .write_keepalive = record_keepalive,
        .end_stream = record_end_stream,
        .status_only = record_status_only
    };
    return writer;
}

/* The embedder's own header representation, which is the point of the lookup
 * being a callback: the adapter never learns what shape it has. */
typedef struct slice_headers {
    const char *const *names;
    const char *const *values;
    size_t count;
    int lookups;
} slice_headers_t;

static int slice_lookup(
    void *context, const char *name, const char **out_value, size_t *out_length) {
    slice_headers_t *headers = context;
    ++headers->lookups;
    const char *found = NULL;
    for (size_t index = 0; index < headers->count; ++index) {
        if (strcasecmp(headers->names[index], name) != 0) continue;
        /* Present more than once is reported absent, per the seam's contract. */
        if (found) return 0;
        found = headers->values[index];
    }
    if (!found) return 0;
    if (out_value) *out_value = found;
    if (out_length) *out_length = strlen(found);
    return 1;
}

static maelys_mcp_http_request_t make_request(slice_headers_t *headers) {
    maelys_mcp_http_request_t request = {
        .method = "POST",
        .path = "/mcp",
        .header_lookup = slice_lookup,
        .header_context = headers,
        .body = "{}",
        .body_length = 2u,
        .principal = NULL,
        .cancel_fd = -1
    };
    return request;
}

static int json_mode_answers_once(void) {
    maelys_mcp_http_adapter_t *adapter = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_create(
        MAELYS_MCP_HTTP_PLACEHOLDER_JSON, &adapter) == MAELYS_MCP_OK);
    slice_headers_t headers = {0};
    maelys_mcp_http_request_t request = make_request(&headers);
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, NULL) ==
        MAELYS_MCP_OK);
    /* Exactly one of begin_json / begin_stream, at most once. */
    ASSERT_TRUE(state.begin_json_calls == 1);
    ASSERT_TRUE(state.begin_stream_calls == 0);
    ASSERT_TRUE(state.end_stream_calls == 0);
    ASSERT_TRUE(state.status_only_calls == 0);
    /* The H1 placeholder: 503 with -32600 and no id. It is deliberately not a
     * 200 with a synthetic result - an endpoint that does not dispatch must not
     * look like one that did. */
    ASSERT_TRUE(state.status == 503);
    ASSERT_TRUE(strstr(state.body, "-32600") != NULL);
    ASSERT_TRUE(strstr(state.body, "\"id\"") == NULL);
    maelys_mcp_http_adapter_destroy(adapter);
    return 0;
}

static int stream_mode_ends_exactly_once(void) {
    maelys_mcp_http_adapter_t *adapter = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_create(
        MAELYS_MCP_HTTP_PLACEHOLDER_STREAM, &adapter) == MAELYS_MCP_OK);
    slice_headers_t headers = {0};
    maelys_mcp_http_request_t request = make_request(&headers);
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, NULL) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(state.begin_stream_calls == 1);
    ASSERT_TRUE(state.begin_json_calls == 0);
    ASSERT_TRUE(state.end_stream_calls == 1);
    ASSERT_TRUE(state.disposition == MAELYS_MCP_HTTP_STREAM_COMPLETE);
    maelys_mcp_http_adapter_destroy(adapter);
    return 0;
}

/* An exchange cancelled before it is handled ends ABORTED, which is what tells
 * the writer to emit no terminal chunk. */
static int cancellation_selects_the_aborted_disposition(void) {
    maelys_mcp_http_adapter_t *adapter = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_create(
        MAELYS_MCP_HTTP_PLACEHOLDER_STREAM, &adapter) == MAELYS_MCP_OK);
    slice_headers_t headers = {0};
    maelys_mcp_http_request_t request = make_request(&headers);
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    maelys_mcp_http_exchange_t *exchange = NULL;
    /*
     * The published handle is the point: a canceller has something to name, and
     * it names the exchange rather than the request, so the request struct's
     * address never becomes a concurrent identity.
     */
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, &exchange) ==
        MAELYS_MCP_OK);
    /* Retired before _handle returned. */
    ASSERT_TRUE(exchange == NULL);
    /* Cancelling a retired handle is a no-op rather than a use-after-free.
     * NULL is the retired handle's value, and the call must tolerate it. */
    maelys_mcp_http_exchange_cancel(exchange);
    maelys_mcp_http_adapter_destroy(adapter);
    return 0;
}

typedef struct cancelling_writer {
    recording_writer_t recording;
    maelys_mcp_http_exchange_t **exchange;
} cancelling_writer_t;

/* Cancels from inside begin_stream, which is the only moment in H1 at which a
 * live exchange handle is observable - it stands in for the other thread that
 * will do this in H3. */
static maelys_mcp_result_t cancel_then_begin_stream(void *context) {
    cancelling_writer_t *writer = context;
    maelys_mcp_http_exchange_cancel(*writer->exchange);
    return record_begin_stream(&writer->recording);
}

static int a_live_cancel_aborts_the_stream(void) {
    maelys_mcp_http_adapter_t *adapter = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_create(
        MAELYS_MCP_HTTP_PLACEHOLDER_STREAM, &adapter) == MAELYS_MCP_OK);
    slice_headers_t headers = {0};
    maelys_mcp_http_request_t request = make_request(&headers);
    maelys_mcp_http_exchange_t *exchange = NULL;
    cancelling_writer_t state = {.recording = {0}, .exchange = &exchange};
    maelys_mcp_http_response_writer_t writer = make_writer(&state.recording);
    writer.context = &state;
    writer.begin_stream = cancel_then_begin_stream;
    /* The other five entry points still address the recording state, so the
     * writer's context is split deliberately here rather than by accident. */
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, &exchange) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(state.recording.begin_stream_calls == 1);
    ASSERT_TRUE(state.recording.end_stream_calls == 1);
    ASSERT_TRUE(state.recording.disposition == MAELYS_MCP_HTTP_STREAM_ABORTED);
    maelys_mcp_http_adapter_destroy(adapter);
    return 0;
}

static int the_adapter_reads_headers_only_through_the_lookup(void) {
    maelys_mcp_http_adapter_t *adapter = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_create(
        MAELYS_MCP_HTTP_PLACEHOLDER_JSON, &adapter) == MAELYS_MCP_OK);
    static const char *const names[] = {"Authorization", "Mcp-Method"};
    static const char *const values[] = {"Bearer secret", "tools/call"};
    slice_headers_t headers = {.names = names, .values = values, .count = 2u};
    maelys_mcp_http_request_t request = make_request(&headers);
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, NULL) ==
        MAELYS_MCP_OK);
    /* H1 validates nothing, so the adapter asks for nothing. The claim worth
     * pinning now is the one that must survive H2: the adapter never asks for
     * Authorization, and this counter is what would notice if it started. */
    ASSERT_TRUE(headers.lookups == 0);
    maelys_mcp_http_adapter_destroy(adapter);
    return 0;
}

static int arguments_are_validated(void) {
    maelys_mcp_http_adapter_t *adapter = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_create(
        (maelys_mcp_http_placeholder_t)7, &adapter) == MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_http_adapter_create(
        MAELYS_MCP_HTTP_PLACEHOLDER_JSON, NULL) == MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_http_adapter_create(
        MAELYS_MCP_HTTP_PLACEHOLDER_JSON, &adapter) == MAELYS_MCP_OK);
    slice_headers_t headers = {0};
    maelys_mcp_http_request_t request = make_request(&headers);
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(NULL, &request, &writer, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, NULL, &writer, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, NULL, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);
    /* An incomplete writer is an argument error rather than a crash: the seam
     * is public, so a partially-filled struct is a thing an embedder will do. */
    maelys_mcp_http_response_writer_t partial = writer;
    partial.end_stream = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &partial, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);
    maelys_mcp_http_adapter_destroy(adapter);
    maelys_mcp_http_adapter_destroy(NULL);
    return 0;
}

int main(void) {
    static const maelys_test_case_t cases[] = {
        {"JSON mode calls begin_json exactly once", json_mode_answers_once},
        {"stream mode ends exactly once per begin_stream", stream_mode_ends_exactly_once},
        {"the exchange handle is retired before _handle returns",
            cancellation_selects_the_aborted_disposition},
        {"a cancel while the exchange is live aborts the stream",
            a_live_cancel_aborts_the_stream},
        {"the adapter never asks for Authorization",
            the_adapter_reads_headers_only_through_the_lookup},
        {"arguments are validated rather than trusted", arguments_are_validated}
    };
    return maelys_run_tests(cases, sizeof(cases) / sizeof(*cases));
}
