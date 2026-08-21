/*
 * The adapter seam, driven by a recording writer with no socket involved.
 *
 * This is one half of H1's merge criterion and the reason the seam is a
 * function rather than a descriptor: an orchestrator behind its own listener,
 * and a fuzz target with no listener at all, reach the adapter exactly the way
 * this file does.
 *
 * H1's half asserts the CONTRACT rather than the placeholder answer's content:
 * exactly one of begin_json/begin_stream, end_stream exactly once per
 * begin_stream, the exchange published before any work and retired before
 * _handle returns, and a cancel after the return that is a no-op.
 *
 * H2's half is the REJECTION MATRIX below - one named case per MCP-header rule
 * in docs/http-transport-design.md, each carrying the rule it exists for in its
 * `rule` column so a reader can check the matrix against the design rather than
 * against the implementation. Every case is a negative one except where a
 * positive is what proves a rule does NOT fire.
 *
 * H3's half is everything that needs a RUNTIME behind the seam: mode selection
 * on the first frame, the ordering that SSE depends on, 202 for a notification,
 * the status rows dispatch produces, the principal's lifetime, and the two ways
 * an exchange ends early. It still involves no socket - a recording writer and
 * a pipe are the whole apparatus - which is what makes these the same cases an
 * orchestrator behind its own listener would exercise.
 */
#include "maelys/mcp.h"
#include "maelys/mcp/http.h"
#include "tests/test_support.h"

#include <jansson.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MAX_EVENTS 16

typedef struct recording_writer {
    int begin_json_calls;
    int begin_stream_calls;
    int event_calls;
    int keepalive_calls;
    int end_stream_calls;
    int status_only_calls;
    int status;
    maelys_mcp_http_stream_end_t disposition;
    char body[1024];
    size_t body_length;
    /*
     * Every SSE event, in the order the adapter wrote it. Kept as text and
     * parsed by the test rather than parsed here, because the ordering claim is
     * about the SEQUENCE of bytes that reached the wire, and a recorder that
     * normalized them would be able to hide a reordering.
     */
    char events[MAX_EVENTS][512];
    int event_count;
    /* Set when end_stream is called, so a test can assert that no event was
     * written after the stream ended rather than only counting. */
    int events_at_end;
} recording_writer_t;

static maelys_mcp_result_t record_begin_json(
    void *context, int status, const char *body, size_t length) {
    recording_writer_t *writer = context;
    ++writer->begin_json_calls;
    writer->status = status;
    if (length < sizeof(writer->body)) {
        memcpy(writer->body, body, length);
        writer->body[length] = '\0';
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
    ++writer->event_calls;
    if (writer->event_count < MAX_EVENTS &&
        length < sizeof(writer->events[0])) {
        memcpy(writer->events[writer->event_count], json, length);
        writer->events[writer->event_count][length] = '\0';
        ++writer->event_count;
    }
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
    writer->events_at_end = writer->event_count;
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

/*
 * The embedder's own header representation, which is the point of the lookup
 * being a callback: the adapter never learns what shape it has.
 *
 * `asked` records every name the adapter asked for, which is what lets a test
 * assert a negative - that Authorization is never among them.
 */
#define MAX_ASKED 16

typedef struct slice_headers {
    /* Flat name/value pairs, NULL-terminated. */
    const char *const *pairs;
    int lookups;
    const char *asked[MAX_ASKED];
    int asked_count;
} slice_headers_t;

static int slice_lookup(
    void *context, const char *name, const char **out_value, size_t *out_length) {
    slice_headers_t *headers = context;
    ++headers->lookups;
    if (headers->asked_count < MAX_ASKED) headers->asked[headers->asked_count++] = name;
    const char *found = NULL;
    for (size_t index = 0u; headers->pairs && headers->pairs[index]; index += 2u) {
        if (strcasecmp(headers->pairs[index], name) != 0) continue;
        /* Present more than once is reported absent, per the seam's contract. */
        if (found) return 0;
        found = headers->pairs[index + 1u];
    }
    if (!found) return 0;
    if (out_value) *out_value = found;
    if (out_length) *out_length = strlen(found);
    return 1;
}

static int asked_for(const slice_headers_t *headers, const char *name) {
    for (int index = 0; index < headers->asked_count; ++index) {
        if (strcasecmp(headers->asked[index], name) == 0) return 1;
    }
    return 0;
}

static maelys_mcp_http_request_t make_request(
    slice_headers_t *headers, const char *body) {
    maelys_mcp_http_request_t request = {
        .method = "POST",
        .path = "/mcp",
        .header_lookup = slice_lookup,
        .header_context = headers,
        .body = body,
        .body_length = strlen(body),
        .principal = NULL,
        .cancel_fd = -1,
        .shutdown_fd = -1
    };
    return request;
}

/* ------------------------------------------------------------- the runtime */

/*
 * Two runtimes, because the two halves of this file want opposite things. The
 * matrix wants a runtime with NO modules, so that "reached dispatch" is a
 * single cheap status; the H3 cases want tools, resources and subscriptions,
 * because the properties they check are about what those produce.
 */
static maelys_mcp_runtime_t *bare_runtime(void) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "http-adapter-test", .server_version = "1.0"
    };
    maelys_mcp_runtime_t *runtime = NULL;
    return maelys_mcp_runtime_create(&config, &runtime) == MAELYS_MCP_OK ?
        runtime : NULL;
}

typedef struct tool_state {
    int calls;
    int progress_reports;
    /* Raised once the provider is inside the call and has reported its first
     * progress frame, so a test can cancel at a moment it actually chose
     * rather than at one it hoped for. */
    int entered_write;
    /* Read by the provider; it does not return until this is readable, which
     * is how a "wedged" call is expressed without a sleep. */
    int hold_read;
} tool_state_t;

static maelys_mcp_result_t emitting_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    (void)out_error;
    tool_state_t *state = context;
    state->calls++;
    for (int step = 0; step < 3; ++step) {
        if (maelys_mcp_provider_report_progress(request->progress,
                step * 33.0, 100.0, NULL) == MAELYS_MCP_OK && request->progress) {
            state->progress_reports++;
        }
        if (step == 0 && state->entered_write >= 0) {
            ssize_t written = write(state->entered_write, "x", 1u);
            (void)written;
        }
    }
    if (state->hold_read >= 0) {
        /* The wedged provider. It blocks until the test lets it go, which is
         * what makes "the connection slot is freed while the channel is not"
         * observable rather than timing-dependent. */
        struct pollfd descriptor = {.fd = state->hold_read, .events = POLLIN, .revents = 0};
        (void)poll(&descriptor, 1u, 5000);
    }
    out_result->structured_content = json_pack("{s:s}", "tool", request->tool_name);
    return out_result->structured_content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static maelys_mcp_result_t fixture_read_resource(
    void *context,
    const maelys_mcp_resource_request_t *request,
    maelys_mcp_resource_result_t *out_result,
    char **out_error) {
    (void)context;
    (void)out_error;
    out_result->contents = json_pack("[{s:s,s:s,s:s}]",
        "uri", request->uri, "mimeType", "text/plain", "text", "body");
    return out_result->contents ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static maelys_mcp_runtime_t *serving_runtime(tool_state_t *state) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "http-adapter-test", .server_version = "1.0"
    };
    maelys_mcp_runtime_t *runtime = NULL;
    if (maelys_mcp_runtime_create(&config, &runtime) != MAELYS_MCP_OK) return NULL;
    if (maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_TOOLS) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_RESOURCES) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_enable_module(runtime,
            MAELYS_MCP_MODULE_SUBSCRIPTIONS) != MAELYS_MCP_OK) {
        (void)maelys_mcp_runtime_destroy(runtime);
        return NULL;
    }
    json_t *schema = json_pack("{s:s,s:b}", "type", "object", "additionalProperties", 0);
    if (!schema) {
        (void)maelys_mcp_runtime_destroy(runtime);
        return NULL;
    }
    maelys_mcp_tool_t tools[] = {{
        .name = "fx.stream",
        .title = "Stream",
        .description = "A tool that reports progress.",
        .input_schema = schema,
        .effect = MAELYS_MCP_EFFECT_READ
    }};
    static const maelys_mcp_resource_t resources[] = {{
        .uri = "fx://repo/doc.txt", .name = "Doc", .mime_type = "text/plain"
    }};
    maelys_mcp_provider_config_t provider_config = {
        .name = "fixture",
        .version = "1",
        .tools = tools,
        .tool_count = 1,
        .resources = resources,
        .resource_count = 1,
        .call = emitting_call,
        .read_resource = fixture_read_resource,
        .context = state
    };
    maelys_mcp_provider_t *provider = NULL;
    maelys_mcp_result_t status = maelys_mcp_provider_create(&provider_config, &provider);
    json_decref(schema);
    if (status != MAELYS_MCP_OK) {
        (void)maelys_mcp_runtime_destroy(runtime);
        return NULL;
    }
    if (maelys_mcp_runtime_add_provider(runtime, provider, NULL) != MAELYS_MCP_OK) {
        maelys_mcp_provider_destroy(provider);
        (void)maelys_mcp_runtime_destroy(runtime);
        return NULL;
    }
    return runtime;
}

static maelys_mcp_http_adapter_t *adapter_on(maelys_mcp_runtime_t *runtime) {
    maelys_mcp_http_adapter_config_t config = {
        .runtime = runtime,
        /* Short enough that a shutdown case finishes inside the suite and long
         * enough that a loopback-free in-process dispatch never trips it. */
        .close_timeout_ms = 1000u,
        .keepalive_interval_ms = 50u
    };
    maelys_mcp_http_adapter_t *adapter = NULL;
    return maelys_mcp_http_adapter_create(&config, &adapter) == MAELYS_MCP_OK ?
        adapter : NULL;
}

/* The `method` of an SSE event this recorder captured, for the ordering
 * assertions. NULL when the frame is a response rather than a notification. */
static int event_method_is(const recording_writer_t *state, int index,
    const char *expected) {
    json_t *frame = json_loads(state->events[index], 0, NULL);
    if (!frame) return 0;
    json_t *method = json_object_get(frame, "method");
    int matches = json_is_string(method) &&
        strcmp(json_string_value(method), expected) == 0;
    json_decref(frame);
    return matches;
}

static int event_is_response_with_id(const recording_writer_t *state, int index,
    json_int_t id) {
    json_t *frame = json_loads(state->events[index], 0, NULL);
    if (!frame) return 0;
    int matches = json_object_get(frame, "method") == NULL &&
        json_integer_value(json_object_get(frame, "id")) == id &&
        (json_object_get(frame, "result") || json_object_get(frame, "error"));
    json_decref(frame);
    return matches;
}

/* ------------------------------------------------ the seam contract half */

/*
 * The modern `_meta` every body here carries. clientCapabilities is not
 * optional on a modern request, so a body without it is refused for a reason
 * that has nothing to do with what these cases are testing.
 */
#define MODERN_META \
    "\"_meta\":{" \
    "\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"," \
    "\"io.modelcontextprotocol/clientInfo\":{\"name\":\"t\",\"version\":\"1\"}," \
    "\"io.modelcontextprotocol/clientCapabilities\":{}}"

#define VALID_BODY \
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{" \
    MODERN_META "}}"

static const char *const VALID_HEADERS[] = {
    "MCP-Protocol-Version", "2026-07-28",
    "Mcp-Method", "tools/list",
    NULL
};

/* A tools/call that asks for progress, which is what makes its first frame a
 * notification and therefore selects the stream. */
#define STREAMING_BODY \
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{" \
    "\"name\":\"fx.stream\",\"arguments\":{}," \
    "\"_meta\":{" \
    "\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"," \
    "\"io.modelcontextprotocol/clientInfo\":{\"name\":\"t\",\"version\":\"1\"}," \
    "\"io.modelcontextprotocol/clientCapabilities\":{}," \
    "\"progressToken\":\"tok-1\"}}}"

static const char *const STREAMING_HEADERS[] = {
    "MCP-Protocol-Version", "2026-07-28",
    "Mcp-Method", "tools/call",
    "Mcp-Name", "fx.stream",
    NULL
};

/*
 * Mode selection, first half: the first frame IS this request's response, so
 * the reply is application/json and that frame is the whole body. No stream is
 * ever begun, which is the property that makes a plain request cost no chunked
 * framing at all.
 */
static int json_mode_answers_once(void) {
    tool_state_t tools = {.entered_write = -1, .hold_read = -1};
    maelys_mcp_runtime_t *runtime = serving_runtime(&tools);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_http_adapter_t *adapter = adapter_on(runtime);
    ASSERT_TRUE(adapter != NULL);
    slice_headers_t headers = {.pairs = VALID_HEADERS};
    maelys_mcp_http_request_t request = make_request(&headers, VALID_BODY);
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, NULL) ==
        MAELYS_MCP_OK);
    /* Exactly one of begin_json / begin_stream, at most once. */
    ASSERT_TRUE(state.begin_json_calls == 1);
    ASSERT_TRUE(state.begin_stream_calls == 0);
    ASSERT_TRUE(state.end_stream_calls == 0);
    ASSERT_TRUE(state.status_only_calls == 0);
    /* 200 and a real catalogue, not a placeholder. The tool the fixture
     * registered is in it, which is what "the HTTP endpoint serves MCP" means
     * at its smallest. */
    ASSERT_TRUE(state.status == 200);
    ASSERT_TRUE(strstr(state.body, "fx.stream") != NULL);
    ASSERT_TRUE(strstr(state.body, "\"id\":1") != NULL);
    maelys_mcp_http_adapter_destroy(adapter);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * Mode selection, second half, and the ordering proof this transport exists to
 * preserve. The first frame is a progress notification, so the reply is a
 * stream; every progress frame precedes the response; the response is last; and
 * end_stream(COMPLETE) is called exactly once, after it.
 *
 * The last of those is not decoration. Over SSE the final response terminates
 * the stream, so a notification behind it is not late - it is dropped outright,
 * which is why tests/test_middleware.c states the same ordering one layer down
 * as test_wrap_sink_keeps_progress_ahead_of_the_response.
 */
static int stream_mode_keeps_progress_ahead_of_the_response(void) {
    tool_state_t tools = {.entered_write = -1, .hold_read = -1};
    maelys_mcp_runtime_t *runtime = serving_runtime(&tools);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_http_adapter_t *adapter = adapter_on(runtime);
    ASSERT_TRUE(adapter != NULL);
    slice_headers_t headers = {.pairs = STREAMING_HEADERS};
    maelys_mcp_http_request_t request = make_request(&headers, STREAMING_BODY);
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, NULL) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(state.begin_stream_calls == 1);
    ASSERT_TRUE(state.begin_json_calls == 0);
    ASSERT_TRUE(state.end_stream_calls == 1);
    ASSERT_TRUE(state.disposition == MAELYS_MCP_HTTP_STREAM_COMPLETE);
    ASSERT_TRUE(tools.progress_reports == 3);
    /* Three progress frames, then the response, and nothing after it. */
    ASSERT_TRUE(state.event_count == 4);
    ASSERT_TRUE(event_method_is(&state, 0, "notifications/progress"));
    ASSERT_TRUE(event_method_is(&state, 1, "notifications/progress"));
    ASSERT_TRUE(event_method_is(&state, 2, "notifications/progress"));
    ASSERT_TRUE(event_is_response_with_id(&state, 3, 1));
    /* The terminal chunk came after every event, not in the middle of them. */
    ASSERT_TRUE(state.events_at_end == 4);
    maelys_mcp_http_adapter_destroy(adapter);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

static int the_exchange_handle_is_retired(void) {
    tool_state_t tools = {.entered_write = -1, .hold_read = -1};
    maelys_mcp_runtime_t *runtime = serving_runtime(&tools);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_http_adapter_t *adapter = adapter_on(runtime);
    ASSERT_TRUE(adapter != NULL);
    slice_headers_t headers = {.pairs = VALID_HEADERS};
    maelys_mcp_http_request_t request = make_request(&headers, VALID_BODY);
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
    /* Cancelling or shutting down a retired handle is a no-op rather than a
     * use-after-free, and both entry points owe that equally. */
    maelys_mcp_http_exchange_cancel(exchange);
    maelys_mcp_http_exchange_shutdown(exchange);
    maelys_mcp_http_adapter_destroy(adapter);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

typedef struct cancelling_writer {
    recording_writer_t recording;
    maelys_mcp_http_exchange_t **exchange;
} cancelling_writer_t;

/* Cancels from inside the first event's write, which is the earliest moment at
 * which a stream exists to abandon. */
static maelys_mcp_result_t cancel_then_record_event(
    void *context, const char *json, size_t length) {
    cancelling_writer_t *writer = context;
    maelys_mcp_http_exchange_cancel(*writer->exchange);
    return record_event(&writer->recording, json, length);
}

/*
 * A live cancel abandons the stream, and the disposition is what says so. No
 * terminal chunk is written, because a chunked body that ends without one is
 * how HTTP/1.1 spells "truncated" - and writing 0\r\n\r\n would instead tell
 * the client the stream completed normally, silently turning a cancelled call
 * into an apparently-successful empty one.
 */
static int a_live_cancel_aborts_the_stream(void) {
    tool_state_t tools = {.entered_write = -1, .hold_read = -1};
    maelys_mcp_runtime_t *runtime = serving_runtime(&tools);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_http_adapter_t *adapter = adapter_on(runtime);
    ASSERT_TRUE(adapter != NULL);
    slice_headers_t headers = {.pairs = STREAMING_HEADERS};
    maelys_mcp_http_request_t request = make_request(&headers, STREAMING_BODY);
    maelys_mcp_http_exchange_t *exchange = NULL;
    cancelling_writer_t state = {.recording = {0}, .exchange = &exchange};
    maelys_mcp_http_response_writer_t writer = make_writer(&state.recording);
    writer.context = &state;
    writer.write_event = cancel_then_record_event;
    /* ERR_CLOSED rather than OK: a cancelled exchange is reported as one, which
     * is how the server layer learns not to reuse the connection. */
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, &exchange) ==
        MAELYS_MCP_ERR_CLOSED);
    ASSERT_TRUE(state.recording.begin_stream_calls == 1);
    ASSERT_TRUE(state.recording.end_stream_calls == 1);
    ASSERT_TRUE(state.recording.disposition == MAELYS_MCP_HTTP_STREAM_ABORTED);
    /* Exactly one event reached the wire: the one during which the cancel
     * happened. Nothing queued behind it was written on the way out. */
    ASSERT_TRUE(state.recording.event_count == 1);
    maelys_mcp_http_adapter_destroy(adapter);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * H1 pinned this as "lookups == 0", with a comment saying the claim worth
 * keeping was the one that had to survive H2. H2 is here and the adapter now
 * reads headers, so the counter is replaced by the claim itself: whatever it
 * asks for, it never asks for Authorization. The credential is the server
 * layer's and the adapter has no business seeing it.
 */
static int the_adapter_never_asks_for_authorization(void) {
    maelys_mcp_runtime_t *runtime = bare_runtime();
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_http_adapter_t *adapter = adapter_on(runtime);
    ASSERT_TRUE(adapter != NULL);
    static const char *const pairs[] = {
        "Authorization", "Bearer secret",
        "MCP-Protocol-Version", "2026-07-28",
        "Mcp-Method", "tools/call",
        "Mcp-Name", "search",
        NULL
    };
    slice_headers_t headers = {.pairs = pairs};
    maelys_mcp_http_request_t request = make_request(&headers,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"search\"," MODERN_META "}}");
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, NULL) ==
        MAELYS_MCP_OK);
    /* It validated - so it did ask for something. */
    ASSERT_TRUE(headers.lookups > 0);
    ASSERT_TRUE(asked_for(&headers, "MCP-Protocol-Version"));
    ASSERT_TRUE(asked_for(&headers, "Mcp-Method"));
    ASSERT_TRUE(asked_for(&headers, "Mcp-Name"));
    /* And never for this one. */
    ASSERT_TRUE(!asked_for(&headers, "Authorization"));
    maelys_mcp_http_adapter_destroy(adapter);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

static int arguments_are_validated(void) {
    maelys_mcp_runtime_t *runtime = bare_runtime();
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_http_adapter_t *adapter = NULL;
    maelys_mcp_http_adapter_config_t config = {.runtime = runtime};
    /* A config with no runtime is refused rather than accepted and discovered
     * at the first dispatch. The placeholder that used to stand in for a
     * runtime is gone, so there is nothing left for an adapter without one to
     * answer with. */
    maelys_mcp_http_adapter_config_t no_runtime = {.runtime = NULL};
    ASSERT_TRUE(maelys_mcp_http_adapter_create(&no_runtime, &adapter) ==
        MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_http_adapter_create(NULL, &adapter) ==
        MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_http_adapter_create(&config, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_http_adapter_create(&config, &adapter) == MAELYS_MCP_OK);
    slice_headers_t headers = {.pairs = VALID_HEADERS};
    maelys_mcp_http_request_t request = make_request(&headers, VALID_BODY);
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
    /* H2 cannot validate without a lookup, so a request with none is an
     * argument error rather than an exchange that skips the rules. */
    maelys_mcp_http_request_t no_lookup = request;
    no_lookup.header_lookup = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &no_lookup, &writer, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);
    /*
     * Half a refcount pair is refused. One without the other is a leak or a
     * double free depending on which half was supplied, and neither is a thing
     * to discover at the moment a detached channel is freed on a thread the
     * embedder never created.
     */
    maelys_mcp_http_request_t half_pair = request;
    half_pair.principal_retain = NULL;
    half_pair.principal_release = (void (*)(void *, maelys_mcp_principal_t *))(void *)abort;
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &half_pair, &writer, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);
    maelys_mcp_http_adapter_destroy(adapter);
    maelys_mcp_http_adapter_destroy(NULL);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/* ------------------------------------------------ the H2 rejection matrix */

/*
 * `expect_status` 404 means the case passed validation and REACHED DISPATCH -
 * the matrix runs against a runtime with no modules, so the answer to anything
 * that gets that far is "method not found", which is the status table's 404
 * row. 400 means it was refused here, and `expect_code` says by which rule.
 *
 * That the accepting rows changed from 503 to 404 in H3 is the whole visible
 * difference the phase makes to this matrix: the refusals are unchanged, which
 * is the property worth having - dispatch was added underneath the validation
 * without moving any of it.
 */
typedef struct matrix_case {
    const char *rule;      /* the design rule this case exists for */
    const char *name;
    const char *const *headers;
    const char *body;
    int expect_status;
    int expect_code;
    /* 1 when the refusal must echo the request's id (2), 0 when it must be
     * null. Ignored on a 503. */
    int expect_id;
    /* A substring of the refusal message, or NULL. Two rows of the status
     * table share 400 and -32600 - a JSON-RPC response body and everything
     * else - so the code alone cannot tell them apart and the message is what
     * keeps them separate rows rather than one. */
    const char *expect_message;
} matrix_case_t;

/*
 * Full modern metadata, not just the version. H2 only needed the version key,
 * because the header/body comparison was the end of the road; H3 carries these
 * bodies all the way into dispatch, where an incomplete `_meta` would be
 * refused for a reason that has nothing to do with the header rule under test
 * and would make every accepting row report the same status as every other.
 */
#define META \
    "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"," \
    "\"io.modelcontextprotocol/clientCapabilities\":{}}"

static const char *const H_VERSION_ONLY[] = {
    "MCP-Protocol-Version", "2026-07-28", NULL};
static const char *const H_NONE[] = {NULL};
static const char *const H_VERSION_DUPLICATED[] = {
    "MCP-Protocol-Version", "2026-07-28",
    "MCP-Protocol-Version", "2026-07-28", NULL};
static const char *const H_VERSION_WRONG[] = {
    "MCP-Protocol-Version", "2025-11-25", NULL};
static const char *const H_VERSION_LEGACY_AGREEING[] = {
    "MCP-Protocol-Version", "2025-11-25", "Mcp-Method", "tools/list", NULL};
static const char *const H_VERSION_AND_METHOD[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "tools/list", NULL};
/*
 * Header sets that isolate ONE rule: the version rule cannot be the reason a
 * request is refused if a second rule is also firing, and a matrix whose cases
 * all trip two rules at once cannot tell which one is load-bearing. Each of
 * these carries a matching Mcp-Method so the version rule is the only thing
 * left that can refuse.
 */
static const char *const H_METHOD_ONLY[] = {"Mcp-Method", "tools/list", NULL};
static const char *const H_VERSION_WRONG_WITH_METHOD[] = {
    "MCP-Protocol-Version", "2025-11-25", "Mcp-Method", "tools/list", NULL};
static const char *const H_METHOD_WRONG[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "tools/call", NULL};
static const char *const H_METHOD_CASE[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "Tools/List", NULL};
static const char *const H_METHOD_DUPLICATED[] = {
    "MCP-Protocol-Version", "2026-07-28",
    "Mcp-Method", "tools/list", "Mcp-Method", "tools/list", NULL};

#define CALL_HEADERS(name_value) \
    {"MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "tools/call", \
     "Mcp-Name", (name_value), NULL}

static const char *const H_CALL_NO_NAME[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "tools/call", NULL};
static const char *const H_CALL_NAME_OK[] = CALL_HEADERS("search");
static const char *const H_CALL_NAME_WRONG[] = CALL_HEADERS("other");
static const char *const H_CALL_NAME_SENTINEL_OK[] = CALL_HEADERS("=?base64?c2VhcmNo?=");
static const char *const H_CALL_NAME_SENTINEL_WRONG[] = CALL_HEADERS("=?base64?b3RoZXI=?=");
static const char *const H_CALL_NAME_SENTINEL_UTF8[] = CALL_HEADERS("=?base64?c8OpY3VyaXTDqQ==?=");
static const char *const H_CALL_NAME_BAD_ALPHABET[] = CALL_HEADERS("=?base64?c2Vh*mNo?=");
static const char *const H_CALL_NAME_URLSAFE[] = CALL_HEADERS("=?base64?a-b_?=");
static const char *const H_CALL_NAME_BAD_LENGTH[] = CALL_HEADERS("=?base64?c2VhcmNoZ?=");
/*
 * Payload length 6, which is 2 mod 4 - and unlike a length of 1 mod 4 this is
 * the shape where dropping the multiple-of-four check is a HEAP OVERFLOW rather
 * than merely a wrong answer. capacity is (6/4)*3 = 3, and the second group
 * writes a fourth byte before the alphabet test on the byte after it can
 * refuse. The 9-byte case above cannot show that: its second group is fully
 * inside the payload and its third is refused before the write.
 */
static const char *const H_CALL_NAME_BAD_LENGTH_2MOD4[] = CALL_HEADERS("=?base64?c2Vhcm?=");
static const char *const H_CALL_NAME_BAD_PADBITS[] = CALL_HEADERS("=?base64?YR==?=");
static const char *const H_CALL_NAME_BAD_PADBITS_ONE[] = CALL_HEADERS("=?base64?YWJ=?=");
static const char *const H_CALL_NAME_INNER_PAD[] = CALL_HEADERS("=?base64?YQ==YQ==?=");
static const char *const H_CALL_NAME_INVALID_UTF8[] = CALL_HEADERS("=?base64?//4=?=");
static const char *const H_CALL_NAME_EMBEDDED_NUL[] = CALL_HEADERS("=?base64?YQBi?=");
static const char *const H_CALL_NAME_SHORT_PATTERN[] = CALL_HEADERS("=?base64?=");
static const char *const H_CALL_NAME_COLLISION_RAW[] = CALL_HEADERS("=?base64?QQ==?=");
static const char *const H_CALL_NAME_COLLISION_ENCODED[] =
    CALL_HEADERS("=?base64?PT9iYXNlNjQ/UVE9PT89?=");
/*
 * A payload that uses every boundary of the Base64 alphabet - 'A', 'Z', 'a',
 * 'z', '0', '9', '+' and '/' all appear in it - and decodes to clean UTF-8.
 *
 * It exists because mutating base64_value's range tests one boundary at a time
 * (`c >= 'A'` to `c > 'A'`, `c <= 'Z'` to `c < 'Z'`, and so on) SURVIVED a
 * matrix whose every other payload happened to avoid those six characters. A
 * decoder that silently stops recognising 'A' is a decoder that mis-decodes a
 * name, and nothing here noticed.
 */
#define ALPHABET_NAME "<5}RXZP~E?B@ss?pK~kzCKu2g6KL<b<SQ_woiouF8<f"
#define ALPHABET_B64 "PDV9UlhaUH5FP0JAc3M/cEt+a3pDS3UyZzZLTDxiPFNRX3dvaW91Rjg8Zg=="
static const char *const H_CALL_NAME_ALPHABET[] =
    CALL_HEADERS("=?base64?" ALPHABET_B64 "?=");

/*
 * "YWFA" is Base64 for "aa@". This is the same payload with its 'A' - alphabet
 * index 0 - replaced by '*', which is not in the alphabet at all.
 *
 * It exists because mutating base64_value's rejection from `return -1` to
 * `return -0` SURVIVED: -0 is 0, which is a perfectly good alphabet index, so
 * every unrecognised byte would silently decode as 'A'. Every other rejection
 * case in this matrix would still be refused under that mutant - by the
 * comparison rather than by the decoder - so only a payload whose non-alphabet
 * byte decodes, under the mutant, to EXACTLY the body's name can tell the two
 * apart. A correct decoder refuses this; the mutant accepts it.
 */
static const char *const H_CALL_NAME_ZERO_INDEX[] = CALL_HEADERS("=?base64?YWF*?=");
/*
 * "YWI=" is Base64 for "ab", and it is the only ONE-padding-byte payload in
 * this matrix that is supposed to be ACCEPTED. Without it, deleting the
 * `last && padding == 1u` branch is invisible: the decoder would fall through
 * to read '=' as a data byte, reject every one-pad sentinel there is, and no
 * test would be looking at a one-pad sentinel that should have worked.
 */
static const char *const H_CALL_NAME_ONE_PAD[] = CALL_HEADERS("=?base64?YWI=?=");

/* Prefix without suffix: not a sentinel, so it is compared literally. */
static const char *const H_CALL_NAME_PREFIX_ONLY[] = CALL_HEADERS("=?base64?c2VhcmNo");
/* The URL-safe spelling of "fn5+", which is base64 for "~~~". */
static const char *const H_CALL_NAME_URLSAFE_DECODABLE[] = CALL_HEADERS("=?base64?fn5-?=");

static const char *const H_READ_NO_NAME[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "resources/read", NULL};
static const char *const H_READ_NAME_RAW[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "resources/read",
    "Mcp-Name", "file:///tmp/a%20b", NULL};
static const char *const H_READ_NAME_CANONICAL[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "resources/read",
    "Mcp-Name", "file:///tmp/a b", NULL};
static const char *const H_LIST_WITH_NAME[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "tools/list",
    "Mcp-Name", "irrelevant", NULL};

#define REQ(method, params) \
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"" method "\",\"params\":{" params "}}"
#define NOTE(method, params) \
    "{\"jsonrpc\":\"2.0\",\"method\":\"" method "\",\"params\":{" params "}}"

static const matrix_case_t MATRIX[] = {
    /* --- body parse and classification (the -32700 / -32600 adapter rows) --- */
    {"body not valid JSON -> 400 -32700",
     "truncated json", H_VERSION_ONLY, "{\"jsonrpc\":", 400, -32700, 0, NULL},
    {"body valid JSON but not an object -> 400 -32700",
     "array body", H_VERSION_ONLY, "[1,2,3]", 400, -32700, 0, NULL},
    {"body valid JSON but not an object -> 400 -32700",
     "bare string body", H_VERSION_ONLY, "\"hello\"", 400, -32700, 0, NULL},
    {"body valid JSON but not an object -> 400 -32700",
     "empty body", H_VERSION_ONLY, "", 400, -32700, 0, NULL},
    {"body is a JSON-RPC response (has id, no method) -> 400 -32600 no id",
     "response carrying a result", H_VERSION_ONLY,
     "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}", 400, -32600, 0,
     "JSON-RPC response"},
    {"body is a JSON-RPC response (has id, no method) -> 400 -32600 no id",
     "response carrying an error", H_VERSION_ONLY,
     "{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"code\":-1,\"message\":\"x\"}}",
     400, -32600, 0, "JSON-RPC response"},
    {"anything else -> 400",
     "neither method nor id", H_VERSION_ONLY, "{\"jsonrpc\":\"2.0\"}", 400, -32600, 0,
     "not a JSON-RPC request or notification"},
    {"anything else -> 400",
     "method is not a string", H_VERSION_ONLY,
     "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":7}", 400, -32600, 0, NULL},
    {"a notification is a request WITHOUT an id, so an explicit null id is neither",
     "method with a null id", H_VERSION_ONLY,
     "{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"tools/list\"}", 400, -32600, 0, NULL},

    /* --- MCP-Protocol-Version --- */
    {"MCP-Protocol-Version required, exactly once -> 400 -32020",
     "version header absent", H_NONE, REQ("tools/list", META), 400, -32020, 1, NULL},
    {"a repeated protocol header is reported absent and refused, never merged",
     "version header duplicated", H_VERSION_DUPLICATED,
     REQ("tools/list", META), 400, -32020, 1, NULL},
    {"the header must equal the body's _meta version -> 400 -32020",
     "body carries no _meta version", H_VERSION_ONLY,
     REQ("tools/list", "\"a\":1"), 400, -32020, 1, NULL},
    {"the header must equal the body's _meta version -> 400 -32020",
     "header and _meta disagree", H_VERSION_WRONG,
     REQ("tools/list", META), 400, -32020, 1, NULL},
    /*
     * H2 could only assert that this pair was NOT refused here. H3 can assert
     * where it IS refused, which is the stronger claim and the one the status
     * table's "Produced by" column makes: 400 with -32022, from the runtime,
     * because the channel's era mask does not carry the legacy era. The
     * adapter never sees the version's supportedness and never decides it.
     */
    {"an agreeing pair naming an unserved version is refused by the RUNTIME "
     "with -32022, not by the adapter with -32020",
     "legacy version agreeing with the body", H_VERSION_LEGACY_AGREEING,
     "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{\"_meta\":"
     "{\"io.modelcontextprotocol/protocolVersion\":\"2025-11-25\","
     "\"io.modelcontextprotocol/clientCapabilities\":{}}}}",
     400, -32022, 1, NULL},

    /* --- Mcp-Method --- */
    {"Mcp-Method required on requests -> 400 -32020",
     "method header absent on a request", H_VERSION_ONLY,
     REQ("tools/list", META), 400, -32020, 1, NULL},
    {"a repeated protocol header is reported absent and refused",
     "method header duplicated", H_METHOD_DUPLICATED,
     REQ("tools/list", META), 400, -32020, 1, NULL},
    {"Mcp-Method must equal the body's method byte for byte -> 400 -32020",
     "method header names another method", H_METHOD_WRONG,
     REQ("tools/list", META), 400, -32020, 1, NULL},
    {"header VALUES are compared case-sensitively",
     "method header differs only in case", H_METHOD_CASE,
     REQ("tools/list", META), 400, -32020, 1, NULL},
    /* A notification that survives validation is 202 and an empty body, which
     * is the one accepting shape in this matrix that is not a JSON reply. */
    {"Mcp-Method is required on requests, so a notification without one passes",
     "notification with no method header", H_VERSION_ONLY,
     NOTE("notifications/cancelled", META), 202, 0, 0, NULL},
    {"present-but-wrong is a mismatch whatever the body kind",
     "notification with a mismatched method header", H_METHOD_WRONG,
     NOTE("notifications/cancelled", META), 400, -32020, 0, NULL},

    /* --- Mcp-Name --- */
    {"Mcp-Name required on tools/call -> 400 -32020",
     "name header absent on tools/call", H_CALL_NO_NAME,
     REQ("tools/call", "\"name\":\"search\"," META), 400, -32020, 1, NULL},
    {"Mcp-Name required on resources/read -> 400 -32020",
     "name header absent on resources/read", H_READ_NO_NAME,
     REQ("resources/read", "\"uri\":\"file:///tmp/a\"," META), 400, -32020, 1, NULL},
    {"Mcp-Name must equal params.name -> 400 -32020",
     "name header names another tool", H_CALL_NAME_WRONG,
     REQ("tools/call", "\"name\":\"search\"," META), 400, -32020, 1, NULL},
    {"Mcp-Name must equal params.name",
     "literal name matches", H_CALL_NAME_OK,
     REQ("tools/call", "\"name\":\"search\"," META), 404, 0, 0, NULL},
    {"Mcp-Name is required only where the spec requires it; elsewhere there is "
     "no body field to compare against",
     "name header on tools/list is ignored", H_LIST_WITH_NAME,
     REQ("tools/list", META), 404, 0, 0, NULL},

    /* --- the Base64 sentinel --- */
    {"a value with the prefix AND the suffix is decoded before comparison",
     "sentinel decodes and matches", H_CALL_NAME_SENTINEL_OK,
     REQ("tools/call", "\"name\":\"search\"," META), 404, 0, 0, NULL},
    {"decoding happens before comparison, so a decoded mismatch is still a mismatch",
     "sentinel decodes and mismatches", H_CALL_NAME_SENTINEL_WRONG,
     REQ("tools/call", "\"name\":\"search\"," META), 400, -32020, 1, NULL},
    {"the decoded bytes must be valid UTF-8; a multibyte name round-trips",
     "sentinel carrying multibyte UTF-8", H_CALL_NAME_SENTINEL_UTF8,
     REQ("tools/call", "\"name\":\"s\\u00e9curit\\u00e9\"," META), 404, 0, 0, NULL},
    {"non-alphabet characters are a rejection",
     "sentinel with a non-alphabet byte", H_CALL_NAME_BAD_ALPHABET,
     REQ("tools/call", "\"name\":\"search\"," META), 400, -32020, 1,
     "malformed Base64 sentinel"},
    {"there is no URL-safe alphabet",
     "sentinel using - and _", H_CALL_NAME_URLSAFE,
     REQ("tools/call", "\"name\":\"search\"," META), 400, -32020, 1,
     "malformed Base64 sentinel"},
    {"wrong padding length is a rejection",
     "sentinel payload not a multiple of four", H_CALL_NAME_BAD_LENGTH,
     REQ("tools/call", "\"name\":\"search\"," META), 400, -32020, 1,
     "malformed Base64 sentinel"},
    {"wrong padding length is a rejection, at the length where dropping the "
     "check overflows the decode buffer rather than just answering wrongly",
     "sentinel payload two short of a group", H_CALL_NAME_BAD_LENGTH_2MOD4,
     REQ("tools/call", "\"name\":\"search\"," META), 400, -32020, 1,
     "malformed Base64 sentinel"},
    {"non-zero bits in the padding are a rejection",
     "sentinel with non-zero bits under two pad bytes", H_CALL_NAME_BAD_PADBITS,
     REQ("tools/call", "\"name\":\"a\"," META), 400, -32020, 1,
     "malformed Base64 sentinel"},
    {"non-zero bits in the padding are a rejection",
     "sentinel with non-zero bits under one pad byte", H_CALL_NAME_BAD_PADBITS_ONE,
     REQ("tools/call", "\"name\":\"ab\"," META), 400, -32020, 1,
     "malformed Base64 sentinel"},
    {"padding appears only at the tail",
     "sentinel with padding in the middle", H_CALL_NAME_INNER_PAD,
     REQ("tools/call", "\"name\":\"a\"," META), 400, -32020, 1,
     "malformed Base64 sentinel"},
    /*
     * The next two, and the UTF-8 rules generally, are DEFENCE IN DEPTH and
     * cannot be killed by a behavioural test - a mutation that deletes any of
     * them survives this matrix, correctly. The body value is produced by
     * jansson, which refuses to parse a string that is not valid UTF-8 or that
     * carries \u0000, so a decoded value violating one of those rules can never
     * equal the body value and the comparison refuses it anyway. The design
     * says as much for the NUL case: accepting one "could only produce a
     * comparison that can never succeed". The rules stay because rejecting at
     * the decoder is clearer than rejecting at the comparison, and because the
     * http-headers fuzz target covers the memory safety the comparison does
     * not.
     */
    {"the decoded bytes must be valid UTF-8",
     "sentinel decoding to invalid UTF-8", H_CALL_NAME_INVALID_UTF8,
     REQ("tools/call", "\"name\":\"search\"," META), 400, -32020, 1,
     "malformed Base64 sentinel"},
    {"the decoded bytes must contain no NUL",
     "sentinel decoding to an embedded NUL", H_CALL_NAME_EMBEDDED_NUL,
     REQ("tools/call", "\"name\":\"a\"," META), 400, -32020, 1,
     "malformed Base64 sentinel"},
    {"prefix and suffix must not overlap: below 11 bytes the value is literal",
     "the 10-byte pattern is taken literally", H_CALL_NAME_SHORT_PATTERN,
     REQ("tools/call", "\"name\":\"=?base64?=\"," META), 404, 0, 0, NULL},
    {"a literal value matching the pattern must have been encoded by the client; "
     "the mismatch that results is a genuine HeaderMismatch",
     "raw sentinel-looking name is decoded, not taken literally",
     H_CALL_NAME_COLLISION_RAW,
     REQ("tools/call", "\"name\":\"=?base64?QQ==?=\"," META), 400, -32020, 1, NULL},
    {"the same collision, encoded as the client should have encoded it, passes",
     "encoded sentinel-looking name", H_CALL_NAME_COLLISION_ENCODED,
     REQ("tools/call", "\"name\":\"=?base64?QQ==?=\"," META), 404, 0, 0, NULL},

    /* --- resources/read compares the RAW uri --- */
    {"resources/read compares against the raw params.uri, never the "
     "canonicalized form",
     "raw percent-encoded uri matches the raw body value", H_READ_NAME_RAW,
     REQ("resources/read", "\"uri\":\"file:///tmp/a%20b\"," META), 404, 0, 0, NULL},
    {"normalizing before comparing would let two header values both pass",
     "canonicalized uri does not match the raw body value", H_READ_NAME_CANONICAL,
     REQ("resources/read", "\"uri\":\"file:///tmp/a%20b\"," META), 400, -32020, 1, NULL},

    /* --- ordering --- */
    {"parse precedes compare: a body with no fields has nothing to disagree with",
     "unparseable body with mismatched headers too", H_METHOD_WRONG,
     "{not json", 400, -32700, 0, NULL},

    /*
     * --- one rule at a time ---
     *
     * Each of these was added because a mutation of the rule it names SURVIVED
     * the matrix: the case that was supposed to cover the rule was also
     * tripping a second rule, so removing the first changed nothing observable.
     */
    {"MCP-Protocol-Version required, exactly once -> 400 -32020",
     "version absent while every other rule is satisfied", H_METHOD_ONLY,
     REQ("tools/list", META), 400, -32020, 1, NULL},
    {"the header must equal the body's _meta version -> 400 -32020",
     "no _meta version while every other rule is satisfied", H_VERSION_AND_METHOD,
     REQ("tools/list", "\"a\":1"), 400, -32020, 1, NULL},
    {"the header must equal the body's _meta version -> 400 -32020",
     "version disagrees while every other rule is satisfied",
     H_VERSION_WRONG_WITH_METHOD, REQ("tools/list", META), 400, -32020, 1, NULL},
    {"a value carrying the prefix but NOT the suffix is not a sentinel and is "
     "taken literally",
     "prefix without suffix is compared literally", H_CALL_NAME_PREFIX_ONLY,
     REQ("tools/call", "\"name\":\"=?base64?c2VhcmNo\"," META), 404, 0, 0, NULL},
    {"a malformed sentinel is a rejection, never a fallback to a literal compare",
     "malformed payload is not retried as a literal", H_CALL_NAME_BAD_ALPHABET,
     REQ("tools/call", "\"name\":\"=?base64?c2Vh*mNo?=\"," META), 400, -32020, 1,
     "malformed Base64 sentinel"},
    {"there is no URL-safe alphabet",
     "URL-safe spelling that would otherwise decode to the body's name",
     H_CALL_NAME_URLSAFE_DECODABLE,
     REQ("tools/call", "\"name\":\"~~~\"," META), 400, -32020, 1,
     "malformed Base64 sentinel"},

    {"every boundary of the Base64 alphabet decodes to the byte it names",
     "payload using A, Z, a, z, 0, 9, + and /", H_CALL_NAME_ALPHABET,
     REQ("tools/call", "\"name\":\"" ALPHABET_NAME "\"," META), 404, 0, 0, NULL},
    {"one wrong byte out of that payload is still a mismatch",
     "alphabet payload against a different name", H_CALL_NAME_ALPHABET,
     REQ("tools/call", "\"name\":\"" ALPHABET_NAME "x\"," META), 400, -32020, 1, NULL},

    {"a non-alphabet byte is REJECTED, not folded to alphabet index 0",
     "payload whose bad byte would decode to the body's name", H_CALL_NAME_ZERO_INDEX,
     REQ("tools/call", "\"name\":\"aa@\"," META), 400, -32020, 1, NULL},

    {"a one-padding-byte payload decodes and is accepted",
     "sentinel with a single pad byte", H_CALL_NAME_ONE_PAD,
     REQ("tools/call", "\"name\":\"ab\"," META), 404, 0, 0, NULL},

    /* --- the happy path --- */
    {"a request that satisfies every rule reaches dispatch",
     "valid tools/list request", H_VERSION_AND_METHOD,
     REQ("tools/list", META), 404, 0, 0, NULL}
};

static int check_matrix_case(
    maelys_mcp_runtime_t *runtime, const matrix_case_t *test_case) {
    maelys_mcp_http_adapter_t *adapter = adapter_on(runtime);
    if (!adapter) return 1;
    slice_headers_t headers = {.pairs = test_case->headers};
    maelys_mcp_http_request_t request = make_request(&headers, test_case->body);
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    int failed = 0;
    if (maelys_mcp_http_adapter_handle(adapter, &request, &writer, NULL) !=
        MAELYS_MCP_OK) {
        fprintf(stderr, "  [%s] _handle did not return OK\n", test_case->name);
        failed = 1;
        goto done;
    }
    /*
     * Whatever the verdict, the seam's shape holds: the writer is driven
     * exactly once, in JSON mode - or, for the one accepted notification,
     * exactly once through status_only with no body at all. No case in this
     * matrix ever opens a stream, because none of them can produce a frame
     * that is not the response.
     */
    int accepted_notification = test_case->expect_status == 202;
    if (state.begin_stream_calls != 0 ||
        state.begin_json_calls != (accepted_notification ? 0 : 1) ||
        state.status_only_calls != (accepted_notification ? 1 : 0)) {
        fprintf(stderr, "  [%s] writer was not driven exactly once in JSON mode\n",
            test_case->name);
        failed = 1;
        goto done;
    }
    if (state.status != test_case->expect_status) {
        fprintf(stderr, "  [%s] expected status %d, got %d (%s)\n",
            test_case->name, test_case->expect_status, state.status, state.body);
        failed = 1;
        goto done;
    }
    if (test_case->expect_status == 404 || accepted_notification) goto done;

    json_error_t error;
    json_t *body = json_loads(state.body, 0, &error);
    if (!body) {
        fprintf(stderr, "  [%s] refusal body is not JSON: %s\n",
            test_case->name, state.body);
        failed = 1;
        goto done;
    }
    json_t *code = json_object_get(json_object_get(body, "error"), "code");
    if (!json_is_integer(code) ||
        json_integer_value(code) != test_case->expect_code) {
        fprintf(stderr, "  [%s] expected code %d, got %s\n",
            test_case->name, test_case->expect_code, state.body);
        failed = 1;
    }
    /* The id rule: a -32020 can echo the id because the body parsed by the time
     * a header could be compared; the rows above it have nothing to echo. */
    json_t *id = json_object_get(body, "id");
    if (test_case->expect_id) {
        if (!json_is_integer(id) || json_integer_value(id) != 2) {
            fprintf(stderr, "  [%s] expected the request id echoed, got %s\n",
                test_case->name, state.body);
            failed = 1;
        }
    } else if (!json_is_null(id)) {
        fprintf(stderr, "  [%s] expected a null id, got %s\n",
            test_case->name, state.body);
        failed = 1;
    }
    if (test_case->expect_message &&
        !strstr(state.body, test_case->expect_message)) {
        fprintf(stderr, "  [%s] expected the message to carry \"%s\", got %s\n",
            test_case->name, test_case->expect_message, state.body);
        failed = 1;
    }
    json_decref(body);
done:
    maelys_mcp_http_adapter_destroy(adapter);
    return failed;
}

static int the_mcp_header_rejection_matrix(void) {
    maelys_mcp_runtime_t *runtime = bare_runtime();
    ASSERT_TRUE(runtime != NULL);
    int failures = 0;
    for (size_t index = 0u; index < sizeof(MATRIX) / sizeof(*MATRIX); ++index) {
        if (check_matrix_case(runtime, &MATRIX[index]) != 0) {
            fprintf(stderr, "  rule: %s\n", MATRIX[index].rule);
            ++failures;
        }
    }
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    ASSERT_TRUE(failures == 0);
    return 0;
}


/* ------------------------------------------------------ the H3 dispatch half */

/*
 * The status rows dispatch produces, driven end to end rather than asserted
 * against a switch statement. Each row names the code the RUNTIME answers with
 * and the status the transport reports for it, and the whole point of the table
 * is that the transport never rewrites the second from anything but the first.
 */
typedef struct dispatch_case {
    const char *rule;
    const char *name;
    const char *const *headers;
    const char *body;
    int expect_status;
    int expect_code;      /* 0 when a success is expected */
} dispatch_case_t;

static const char *const H_PING[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "ping", NULL};
static const char *const H_INITIALIZE[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "initialize", NULL};
static const char *const H_MISSING_METHOD[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "prompts/get", NULL};
static const char *const H_CALL_MISSING_TOOL[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "tools/call",
    "Mcp-Name", "fx.absent", NULL};

#define BODY(method_name, extra) \
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"" method_name "\",\"params\":{" \
    extra MODERN_META "}}"

static const dispatch_case_t DISPATCH_MATRIX[] = {
    {"everything else, success or error -> 200",
     "ping", H_PING, BODY("ping", ""), 200, 0},
    {"method not found -> 404 -32601, carrying the JSON-RPC body rather than an "
     "empty one, so a client can tell this server from one that does not host "
     "this endpoint at all",
     "prompts/get, which no module implements", H_MISSING_METHOD,
     BODY("prompts/get", "\"name\":\"x\","), 404, -32601},
    /*
     * The era mask, observed from the outside. No bespoke boundary code refuses
     * `initialize` over HTTP: the channel simply does not carry the legacy era,
     * so the runtime answers -32600 - and -32600 from dispatch is an ANSWER,
     * which is the 200 row. A transport that promoted it to a 4xx would be
     * rewriting a dispatch result, which the status table forbids.
     */
    {"initialize on a modern-only channel is refused by the runtime with "
     "-32600 and reported as the answer it is",
     "initialize over HTTP", H_INITIALIZE,
     BODY("initialize", "\"protocolVersion\":\"2025-11-25\","), 200, -32600},
    {"a dispatched error that is not in the table stays 200",
     "tools/call naming a tool that does not exist", H_CALL_MISSING_TOOL,
     BODY("tools/call", "\"name\":\"fx.absent\",\"arguments\":{},"), 200, -32602}
};

static int check_dispatch_case(
    maelys_mcp_runtime_t *runtime, const dispatch_case_t *test_case) {
    maelys_mcp_http_adapter_t *adapter = adapter_on(runtime);
    if (!adapter) return 1;
    slice_headers_t headers = {.pairs = test_case->headers};
    maelys_mcp_http_request_t request = make_request(&headers, test_case->body);
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    int failed = 0;
    if (maelys_mcp_http_adapter_handle(adapter, &request, &writer, NULL) !=
        MAELYS_MCP_OK) {
        fprintf(stderr, "  [%s] _handle did not return OK\n", test_case->name);
        failed = 1;
        goto done;
    }
    /* Every one of these resolves to a single buffered response, so the reply
     * is JSON and no stream is ever begun. */
    if (state.begin_json_calls != 1 || state.begin_stream_calls != 0 ||
        state.status_only_calls != 0) {
        fprintf(stderr, "  [%s] writer was not driven exactly once in JSON mode\n",
            test_case->name);
        failed = 1;
        goto done;
    }
    if (state.status != test_case->expect_status) {
        fprintf(stderr, "  [%s] expected status %d, got %d (%s)\n",
            test_case->name, test_case->expect_status, state.status, state.body);
        failed = 1;
        goto done;
    }
    json_t *body = json_loads(state.body, 0, NULL);
    if (!body) {
        fprintf(stderr, "  [%s] body is not JSON: %s\n", test_case->name, state.body);
        failed = 1;
        goto done;
    }
    json_t *code = json_object_get(json_object_get(body, "error"), "code");
    if (test_case->expect_code) {
        if (!json_is_integer(code) ||
            json_integer_value(code) != test_case->expect_code) {
            fprintf(stderr, "  [%s] expected code %d, got %s\n",
                test_case->name, test_case->expect_code, state.body);
            failed = 1;
        }
    } else if (code) {
        fprintf(stderr, "  [%s] expected a success, got %s\n",
            test_case->name, state.body);
        failed = 1;
    }
    /* Whatever the row, the response answers the request it was made for. */
    if (json_integer_value(json_object_get(body, "id")) != 1) {
        fprintf(stderr, "  [%s] the response does not carry the request id: %s\n",
            test_case->name, state.body);
        failed = 1;
    }
    json_decref(body);
done:
    maelys_mcp_http_adapter_destroy(adapter);
    return failed;
}

static int the_dispatch_status_rows(void) {
    tool_state_t tools = {.entered_write = -1, .hold_read = -1};
    maelys_mcp_runtime_t *runtime = serving_runtime(&tools);
    ASSERT_TRUE(runtime != NULL);
    int failures = 0;
    for (size_t index = 0u;
         index < sizeof(DISPATCH_MATRIX) / sizeof(*DISPATCH_MATRIX); ++index) {
        if (check_dispatch_case(runtime, &DISPATCH_MATRIX[index]) != 0) {
            fprintf(stderr, "  rule: %s\n", DISPATCH_MATRIX[index].rule);
            ++failures;
        }
    }
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    ASSERT_TRUE(failures == 0);
    return 0;
}

/*
 * A notification gets 202 Accepted with an empty body, and no bespoke boundary
 * code decides which notifications are worth dispatching: every one is handed
 * to the runtime, which handles the ones it knows and drops the rest.
 *
 * notifications/initialized is the case worth pinning by name. It is a legacy
 * lifecycle message arriving on a channel that serves only the modern era; the
 * runtime records it against a handshake that never happened and produces no
 * frame, so what reaches the wire is a 202 and nothing else. That is what
 * "falls out naturally from the era mask" means, and it is asserted here rather
 * than assumed so the day it stops falling out is a failing test.
 */
static const char *const H_NOTE_INITIALIZED[] = {
    "MCP-Protocol-Version", "2026-07-28",
    "Mcp-Method", "notifications/initialized", NULL};
static const char *const H_NOTE_CANCELLED[] = {
    "MCP-Protocol-Version", "2026-07-28",
    "Mcp-Method", "notifications/cancelled", NULL};
static const char *const H_NOTE_UNKNOWN[] = {
    "MCP-Protocol-Version", "2026-07-28",
    "Mcp-Method", "notifications/unknown", NULL};

static int a_notification_is_accepted_with_202(void) {
    static const struct {
        const char *name;
        const char *const *headers;
        const char *body;
    } NOTIFICATIONS[] = {
        {"notifications/initialized", H_NOTE_INITIALIZED,
         "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\","
         "\"params\":{" MODERN_META "}}"},
        {"notifications/cancelled", H_NOTE_CANCELLED,
         "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
         "\"params\":{\"requestId\":7," MODERN_META "}}"},
        {"an unrecognized notification", H_NOTE_UNKNOWN,
         "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/unknown\","
         "\"params\":{" MODERN_META "}}"}
    };
    tool_state_t tools = {.entered_write = -1, .hold_read = -1};
    maelys_mcp_runtime_t *runtime = serving_runtime(&tools);
    ASSERT_TRUE(runtime != NULL);
    for (size_t index = 0u;
         index < sizeof(NOTIFICATIONS) / sizeof(*NOTIFICATIONS); ++index) {
        maelys_mcp_http_adapter_t *adapter = adapter_on(runtime);
        ASSERT_TRUE(adapter != NULL);
        slice_headers_t headers = {.pairs = NOTIFICATIONS[index].headers};
        maelys_mcp_http_request_t request =
            make_request(&headers, NOTIFICATIONS[index].body);
        recording_writer_t state = {0};
        maelys_mcp_http_response_writer_t writer = make_writer(&state);
        ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, NULL) ==
            MAELYS_MCP_OK);
        ASSERT_TRUE(state.status_only_calls == 1);
        ASSERT_TRUE(state.status == 202);
        /* Empty body means empty: no JSON reply, no stream, no events. */
        ASSERT_TRUE(state.begin_json_calls == 0);
        ASSERT_TRUE(state.begin_stream_calls == 0);
        ASSERT_TRUE(state.event_calls == 0);
        maelys_mcp_http_adapter_destroy(adapter);
    }
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * A cancelled notification writes nothing either, and this case exists because
 * the fuzz target found that it did.
 *
 * A notification produces its whole reply synchronously, so it never enters the
 * drain loop and nothing would have consulted the cancellation source: the 202
 * went out to a peer that had already gone. Harmless in bytes, and wrong in the
 * only way that matters - it made "after a cancellation, no further bytes are
 * written for that request" a rule with an exception nobody had written down.
 *
 * The notification is still DISPATCHED. Which notifications matter is the
 * runtime's rule, not the transport's, and a peer leaving does not un-send what
 * it already said.
 */
static int a_cancelled_notification_writes_nothing(void) {
    int wake[2] = {-1, -1};
    ASSERT_TRUE(pipe(wake) == 0);
    ssize_t written = write(wake[1], "x", 1u);
    ASSERT_TRUE(written == 1);
    tool_state_t tools = {.entered_write = -1, .hold_read = -1};
    maelys_mcp_runtime_t *runtime = serving_runtime(&tools);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_http_adapter_t *adapter = adapter_on(runtime);
    ASSERT_TRUE(adapter != NULL);
    slice_headers_t headers = {.pairs = H_NOTE_INITIALIZED};
    maelys_mcp_http_request_t request = make_request(&headers,
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\","
        "\"params\":{" MODERN_META "}}");
    request.cancel_fd = wake[0];
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, NULL) ==
        MAELYS_MCP_ERR_CLOSED);
    ASSERT_TRUE(state.status_only_calls == 0);
    ASSERT_TRUE(state.begin_json_calls == 0);
    ASSERT_TRUE(state.begin_stream_calls == 0);
    maelys_mcp_http_adapter_destroy(adapter);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    close(wake[0]);
    close(wake[1]);
    return 0;
}

/* ------------------------------------------------------ the principal bond */

/*
 * A counting principal, which is what turns "released exactly once" from a
 * hope into a number. The principal itself is this struct, so a use after the
 * last release would be a use of memory this test still owns and can inspect.
 */
typedef struct counted_principal {
    int retains;
    int releases;
    /* Set by a middleware, so the test can assert that what the runtime carried
     * as the channel context is the principal itself and not a wrapper. */
    const void *seen_as_channel_context;
} counted_principal_t;

static void counted_retain(void *context, maelys_mcp_principal_t *principal) {
    (void)context;
    ((counted_principal_t *)principal)->retains++;
}

static void counted_release(void *context, maelys_mcp_principal_t *principal) {
    (void)context;
    ((counted_principal_t *)principal)->releases++;
}

static maelys_mcp_authorize_decision_t note_channel_context(
    void *context, const maelys_mcp_authorize_context_t *request) {
    counted_principal_t *principal = context;
    principal->seen_as_channel_context = maelys_mcp_channel_context(request->channel);
    return MAELYS_MCP_AUTHORIZE_ALLOW;
}

/*
 * channel_create with the principal: the ABI 4 context destructor's first real
 * consumer, and the three claims that make it correct.
 *
 * The adapter takes ONE reference of its own - the embedder's is untouched -
 * and gives it back exactly once. The runtime carries the principal itself as
 * the channel context, so middleware sees the caller rather than a wrapper. And
 * the release lands at the real free, which for a channel that closed cleanly
 * is before _handle returns.
 */
static int the_principal_is_retained_once_and_released_once(void) {
    tool_state_t tools = {.entered_write = -1, .hold_read = -1};
    maelys_mcp_runtime_t *runtime = serving_runtime(&tools);
    ASSERT_TRUE(runtime != NULL);
    counted_principal_t principal = {0};
    maelys_mcp_middleware_t middleware = {
        .name = "context-probe",
        .context = &principal,
        .on_authorize = note_channel_context
    };
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_http_adapter_t *adapter = adapter_on(runtime);
    ASSERT_TRUE(adapter != NULL);
    slice_headers_t headers = {.pairs = STREAMING_HEADERS};
    maelys_mcp_http_request_t request = make_request(&headers, STREAMING_BODY);
    request.principal = (maelys_mcp_principal_t *)&principal;
    request.principal_retain = counted_retain;
    request.principal_release = counted_release;
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, NULL) ==
        MAELYS_MCP_OK);
    /* One retain for the channel, and one release when that channel was freed.
     * The embedder's own reference is not part of this count, which is the
     * whole reason there are two of them. */
    ASSERT_TRUE(principal.retains == 1);
    ASSERT_TRUE(principal.releases == 1);
    /* And what the runtime carried was the principal, not a binding wrapper. */
    ASSERT_TRUE(principal.seen_as_channel_context == (const void *)&principal);
    maelys_mcp_http_adapter_destroy(adapter);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    /* Still exactly one after the runtime is gone: nothing released it twice
     * on the way out. */
    ASSERT_TRUE(principal.releases == 1);
    return 0;
}

/*
 * The same claim on the path that made the context destructor necessary. The
 * provider is wedged, so the exchange's bounded close misses its deadline, the
 * channel is detached rather than waited on, and _handle returns while the
 * channel is still alive somewhere else.
 *
 * Two things are asserted at that moment and they are deliberately different:
 * the connection's work is DONE (the call returned inside the close deadline,
 * so no network peer is holding the slot), and the principal has NOT been
 * released yet, because the channel that carries it still exists. Then the
 * provider is let go, the runtime is destroyed - which drains every detached
 * channel - and the release has happened, exactly once.
 */
static int a_detached_channel_releases_the_principal_exactly_once(void) {
    int hold[2] = {-1, -1};
    ASSERT_TRUE(pipe(hold) == 0);
    tool_state_t tools = {.entered_write = -1, .hold_read = hold[0]};
    maelys_mcp_runtime_t *runtime = serving_runtime(&tools);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_http_adapter_config_t config = {
        .runtime = runtime,
        /* Short on purpose: the close is meant to MISS this. */
        .close_timeout_ms = 50u,
        .keepalive_interval_ms = 20u
    };
    maelys_mcp_http_adapter_t *adapter = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_create(&config, &adapter) == MAELYS_MCP_OK);
    counted_principal_t principal = {0};

    /* Cancel as soon as the provider is inside the call, so the exchange ends
     * while the wedged dispatch is still running. */
    int wake[2] = {-1, -1};
    ASSERT_TRUE(pipe(wake) == 0);
    tools.entered_write = wake[1];

    slice_headers_t headers = {.pairs = STREAMING_HEADERS};
    maelys_mcp_http_request_t request = make_request(&headers, STREAMING_BODY);
    request.principal = (maelys_mcp_principal_t *)&principal;
    request.principal_retain = counted_retain;
    request.principal_release = counted_release;
    /* The abstract cancellation source, as a plain pipe: the adapter cannot
     * tell it from a socket's FIN and does not try. */
    request.cancel_fd = wake[0];
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    maelys_mcp_result_t handled =
        maelys_mcp_http_adapter_handle(adapter, &request, &writer, NULL);
    /* The exchange is over and this thread is free, while the provider is
     * demonstrably still inside its call. */
    ASSERT_TRUE(handled == MAELYS_MCP_ERR_CLOSED);
    ASSERT_TRUE(principal.retains == 1);
    ASSERT_TRUE(principal.releases == 0);

    /* Let the provider finish, then drain every detached channel. */
    ssize_t written = write(hold[1], "x", 1u);
    (void)written;
    maelys_mcp_http_adapter_destroy(adapter);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    ASSERT_TRUE(principal.releases == 1);
    close(hold[0]);
    close(hold[1]);
    close(wake[0]);
    close(wake[1]);
    return 0;
}

/* ----------------------------------------------------- shutdown ordering */

/*
 * subscriptions/listen end to end, which is the method the whole "the channel's
 * own sink, not a bespoke one" decision was made for.
 *
 * The acknowledgement travels the COMPLETION path even though it is a JSON-RPC
 * notification with no id, so "is this frame the final response for this
 * request's id?" says no about it and the stream opens - which is the single
 * fact most readers guess wrong and the one a bespoke sink would have got
 * wrong, seeing the acknowledgement and then nothing forever.
 *
 * Then shutdown, and the difference from cancellation is the whole point: the
 * surviving subscription is completed with `resultType: "complete"`, that frame
 * is written, and the stream ends with its terminal chunk. A cancellation here
 * would have written nothing further and left the body truncated.
 */
typedef struct shutdown_writer {
    recording_writer_t recording;
    maelys_mcp_http_exchange_t **exchange;
} shutdown_writer_t;

static maelys_mcp_result_t shutdown_after_the_ack(
    void *context, const char *json, size_t length) {
    shutdown_writer_t *writer = context;
    maelys_mcp_result_t status = record_event(&writer->recording, json, length);
    /* Asked to finish once the acknowledgement is on the wire, which is the
     * moment a real server's stop would find this stream at. */
    if (writer->recording.event_count == 1) {
        maelys_mcp_http_exchange_shutdown(*writer->exchange);
    }
    return status;
}

static int a_listen_stream_completes_on_shutdown(void) {
    tool_state_t tools = {.entered_write = -1, .hold_read = -1};
    maelys_mcp_runtime_t *runtime = serving_runtime(&tools);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_http_adapter_t *adapter = adapter_on(runtime);
    ASSERT_TRUE(adapter != NULL);
    static const char *const listen_headers[] = {
        "MCP-Protocol-Version", "2026-07-28",
        "Mcp-Method", "subscriptions/listen",
        NULL
    };
    static const char listen_body[] =
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"subscriptions/listen\","
        "\"params\":{\"notifications\":{\"resourceSubscriptions\":"
        "[\"fx://repo/doc.txt\"]}," MODERN_META "}}";
    slice_headers_t headers = {.pairs = listen_headers};
    maelys_mcp_http_request_t request = make_request(&headers, listen_body);
    maelys_mcp_http_exchange_t *exchange = NULL;
    shutdown_writer_t state = {.recording = {0}, .exchange = &exchange};
    maelys_mcp_http_response_writer_t writer = make_writer(&state.recording);
    writer.context = &state;
    writer.write_event = shutdown_after_the_ack;
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, &exchange) ==
        MAELYS_MCP_OK);
    /* A stream, because the first frame was not this id's response. */
    ASSERT_TRUE(state.recording.begin_stream_calls == 1);
    ASSERT_TRUE(state.recording.begin_json_calls == 0);
    /* The acknowledgement, then the completion. */
    ASSERT_TRUE(state.recording.event_count == 2);
    ASSERT_TRUE(event_method_is(&state.recording, 0,
        "notifications/subscriptions/acknowledged"));
    ASSERT_TRUE(event_is_response_with_id(&state.recording, 1, 9));
    ASSERT_TRUE(strstr(state.recording.events[1], "\"resultType\":\"complete\"") != NULL);
    /* Ended properly, with its terminal chunk, because the peer is still there
     * and is owed an ending. */
    ASSERT_TRUE(state.recording.end_stream_calls == 1);
    ASSERT_TRUE(state.recording.disposition == MAELYS_MCP_HTTP_STREAM_COMPLETE);
    maelys_mcp_http_adapter_destroy(adapter);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * The same stream, cancelled instead of shut down, and the two dispositions are
 * the whole difference on the wire. No terminal chunk, and no `resultType:
 * "complete"` frame either - after the abort every enqueue fails ERR_CLOSED, so
 * "MUST NOT send any further messages for it" holds by construction rather than
 * by discipline.
 */
static maelys_mcp_result_t cancel_after_the_ack(
    void *context, const char *json, size_t length) {
    shutdown_writer_t *writer = context;
    maelys_mcp_result_t status = record_event(&writer->recording, json, length);
    if (writer->recording.event_count == 1) {
        maelys_mcp_http_exchange_cancel(*writer->exchange);
    }
    return status;
}

static int a_listen_stream_is_truncated_on_cancel(void) {
    tool_state_t tools = {.entered_write = -1, .hold_read = -1};
    maelys_mcp_runtime_t *runtime = serving_runtime(&tools);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_http_adapter_t *adapter = adapter_on(runtime);
    ASSERT_TRUE(adapter != NULL);
    static const char *const listen_headers[] = {
        "MCP-Protocol-Version", "2026-07-28",
        "Mcp-Method", "subscriptions/listen",
        NULL
    };
    static const char listen_body[] =
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"subscriptions/listen\","
        "\"params\":{\"notifications\":{\"resourceSubscriptions\":"
        "[\"fx://repo/doc.txt\"]}," MODERN_META "}}";
    slice_headers_t headers = {.pairs = listen_headers};
    maelys_mcp_http_request_t request = make_request(&headers, listen_body);
    maelys_mcp_http_exchange_t *exchange = NULL;
    shutdown_writer_t state = {.recording = {0}, .exchange = &exchange};
    maelys_mcp_http_response_writer_t writer = make_writer(&state.recording);
    writer.context = &state;
    writer.write_event = cancel_after_the_ack;
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, &exchange) ==
        MAELYS_MCP_ERR_CLOSED);
    ASSERT_TRUE(state.recording.begin_stream_calls == 1);
    /* The acknowledgement and nothing else. */
    ASSERT_TRUE(state.recording.event_count == 1);
    ASSERT_TRUE(state.recording.end_stream_calls == 1);
    ASSERT_TRUE(state.recording.disposition == MAELYS_MCP_HTTP_STREAM_ABORTED);
    maelys_mcp_http_adapter_destroy(adapter);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * A cancellation that arrives before the first frame writes NOTHING AT ALL - no
 * status line, no stream, no body. There is no reply to choose a mode for, and
 * inventing one for a peer that has gone away would be writing to a socket for
 * the sake of writing to it.
 */
static int a_cancel_before_the_first_frame_writes_nothing(void) {
    int wake[2] = {-1, -1};
    ASSERT_TRUE(pipe(wake) == 0);
    /* Readable before the exchange starts, which is the state the socket is in
     * when a client sent its request and then vanished. */
    ssize_t written = write(wake[1], "x", 1u);
    ASSERT_TRUE(written == 1);
    tool_state_t tools = {.entered_write = -1, .hold_read = -1};
    maelys_mcp_runtime_t *runtime = serving_runtime(&tools);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_http_adapter_t *adapter = adapter_on(runtime);
    ASSERT_TRUE(adapter != NULL);
    slice_headers_t headers = {.pairs = STREAMING_HEADERS};
    maelys_mcp_http_request_t request = make_request(&headers, STREAMING_BODY);
    request.cancel_fd = wake[0];
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, NULL) ==
        MAELYS_MCP_ERR_CLOSED);
    ASSERT_TRUE(state.begin_json_calls == 0);
    ASSERT_TRUE(state.begin_stream_calls == 0);
    ASSERT_TRUE(state.event_calls == 0);
    ASSERT_TRUE(state.end_stream_calls == 0);
    ASSERT_TRUE(state.status_only_calls == 0);
    maelys_mcp_http_adapter_destroy(adapter);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    close(wake[0]);
    close(wake[1]);
    return 0;
}

/*
 * maelys_mcp_http_exchange_cancel works even when the embedder supplied a
 * cancellation DESCRIPTOR and never made it readable.
 *
 * The two are not alternatives that happen to overlap. A descriptor is how an
 * embedder that owns a socket cancels; the entry point is how one that does not
 * cancels; and an embedder is free to hold a descriptor and still call the
 * function - a deadline reaper, say, that has no reason to touch the connection
 * thread's pipe. Because no wakeup pipe is allocated when a descriptor was
 * supplied, the recorded flag is then the ONLY signal, and something has to
 * consult it rather than wait for descriptor readiness that will never come.
 *
 * Mutation found this: deleting the flag check at the top of the drain's wait
 * left every other test green, because all of them raise a descriptor. Under
 * that mutant this exchange never ends.
 */
static maelys_mcp_result_t cancel_on_the_second_keepalive(void *context) {
    shutdown_writer_t *writer = context;
    maelys_mcp_result_t status = record_keepalive(&writer->recording);
    if (writer->recording.keepalive_calls == 2) {
        maelys_mcp_http_exchange_cancel(*writer->exchange);
    }
    return status;
}

static int an_out_of_band_cancel_works_without_a_readable_descriptor(void) {
    /* Supplied and never raised, which is what makes the flag load-bearing. */
    int quiet[2] = {-1, -1};
    ASSERT_TRUE(pipe(quiet) == 0);
    tool_state_t tools = {.entered_write = -1, .hold_read = -1};
    maelys_mcp_runtime_t *runtime = serving_runtime(&tools);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_http_adapter_config_t config = {
        .runtime = runtime,
        .close_timeout_ms = 1000u,
        .keepalive_interval_ms = 10u
    };
    maelys_mcp_http_adapter_t *adapter = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_create(&config, &adapter) == MAELYS_MCP_OK);
    static const char *const listen_headers[] = {
        "MCP-Protocol-Version", "2026-07-28",
        "Mcp-Method", "subscriptions/listen",
        NULL
    };
    static const char listen_body[] =
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"subscriptions/listen\","
        "\"params\":{\"notifications\":{\"resourceSubscriptions\":"
        "[\"fx://repo/doc.txt\"]}," MODERN_META "}}";
    slice_headers_t headers = {.pairs = listen_headers};
    maelys_mcp_http_request_t request = make_request(&headers, listen_body);
    request.cancel_fd = quiet[0];
    maelys_mcp_http_exchange_t *exchange = NULL;
    shutdown_writer_t state = {.recording = {0}, .exchange = &exchange};
    maelys_mcp_http_response_writer_t writer = make_writer(&state.recording);
    writer.context = &state;
    writer.write_keepalive = cancel_on_the_second_keepalive;
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, &exchange) ==
        MAELYS_MCP_ERR_CLOSED);
    ASSERT_TRUE(state.recording.end_stream_calls == 1);
    ASSERT_TRUE(state.recording.disposition == MAELYS_MCP_HTTP_STREAM_ABORTED);
    maelys_mcp_http_adapter_destroy(adapter);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    close(quiet[0]);
    close(quiet[1]);
    return 0;
}

/*
 * The keep-alive comment, which is the one thing an idle stream costs. It is
 * written only once a stream exists - there is nothing to keep alive before the
 * reply's mode is chosen, and a JSON reply has nowhere to put one - and it is
 * written on an IDLE poll rather than on a clock, so an active stream never
 * pays for it.
 */
static maelys_mcp_result_t shutdown_after_two_keepalives(void *context) {
    shutdown_writer_t *writer = context;
    maelys_mcp_result_t status = record_keepalive(&writer->recording);
    if (writer->recording.keepalive_calls == 2) {
        maelys_mcp_http_exchange_shutdown(*writer->exchange);
    }
    return status;
}

static int an_idle_stream_is_kept_alive(void) {
    tool_state_t tools = {.entered_write = -1, .hold_read = -1};
    maelys_mcp_runtime_t *runtime = serving_runtime(&tools);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_http_adapter_config_t config = {
        .runtime = runtime,
        .close_timeout_ms = 1000u,
        /* Short enough that the suite observes two without waiting. */
        .keepalive_interval_ms = 10u
    };
    maelys_mcp_http_adapter_t *adapter = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_create(&config, &adapter) == MAELYS_MCP_OK);
    static const char *const listen_headers[] = {
        "MCP-Protocol-Version", "2026-07-28",
        "Mcp-Method", "subscriptions/listen",
        NULL
    };
    static const char listen_body[] =
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"subscriptions/listen\","
        "\"params\":{\"notifications\":{\"resourceSubscriptions\":"
        "[\"fx://repo/doc.txt\"]}," MODERN_META "}}";
    slice_headers_t headers = {.pairs = listen_headers};
    maelys_mcp_http_request_t request = make_request(&headers, listen_body);
    maelys_mcp_http_exchange_t *exchange = NULL;
    shutdown_writer_t state = {.recording = {0}, .exchange = &exchange};
    maelys_mcp_http_response_writer_t writer = make_writer(&state.recording);
    writer.context = &state;
    writer.write_keepalive = shutdown_after_two_keepalives;
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, &exchange) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(state.recording.keepalive_calls == 2);
    /* The acknowledgement came first and no keep-alive preceded it: nothing is
     * kept alive before there is a stream. */
    ASSERT_TRUE(state.recording.event_count >= 1);
    ASSERT_TRUE(event_method_is(&state.recording, 0,
        "notifications/subscriptions/acknowledged"));
    ASSERT_TRUE(state.recording.end_stream_calls == 1);
    ASSERT_TRUE(state.recording.disposition == MAELYS_MCP_HTTP_STREAM_COMPLETE);
    maelys_mcp_http_adapter_destroy(adapter);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

int main(void) {
    static const maelys_test_case_t cases[] = {
        {"the first frame being the response selects JSON mode",
            json_mode_answers_once},
        {"a stream keeps progress ahead of the response and ends after it",
            stream_mode_keeps_progress_ahead_of_the_response},
        {"the exchange handle is retired before _handle returns",
            the_exchange_handle_is_retired},
        {"a cancel while the exchange is live aborts the stream",
            a_live_cancel_aborts_the_stream},
        {"a cancel before the first frame writes nothing at all",
            a_cancel_before_the_first_frame_writes_nothing},
        {"the adapter never asks for Authorization",
            the_adapter_never_asks_for_authorization},
        {"arguments are validated rather than trusted", arguments_are_validated},
        {"every MCP-header rule refuses what it says it refuses",
            the_mcp_header_rejection_matrix},
        {"every dispatch status row reports what dispatch decided",
            the_dispatch_status_rows},
        {"a notification is accepted with 202 and an empty body",
            a_notification_is_accepted_with_202},
        {"a cancelled notification writes nothing at all",
            a_cancelled_notification_writes_nothing},
        {"an out-of-band cancel works without a readable descriptor",
            an_out_of_band_cancel_works_without_a_readable_descriptor},
        {"the principal is retained once and released once",
            the_principal_is_retained_once_and_released_once},
        {"a detached channel releases the principal exactly once, later",
            a_detached_channel_releases_the_principal_exactly_once},
        {"a listen stream completes with resultType complete on shutdown",
            a_listen_stream_completes_on_shutdown},
        {"a listen stream is truncated without a terminal chunk on cancel",
            a_listen_stream_is_truncated_on_cancel},
        {"an idle stream is kept alive and never before it exists",
            an_idle_stream_is_kept_alive}
    };
    return maelys_run_tests(cases, sizeof(cases) / sizeof(*cases));
}
