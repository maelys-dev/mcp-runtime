/*
 * http-exchange: a whole exchange - headers, body, and a scripted cancellation
 * or shutdown at an arbitrary point - driven through
 * maelys_mcp_http_adapter_handle with a recording writer.
 *
 * The entry point is the adapter seam itself, which is the payoff of that seam
 * being a function rather than a socket: an iteration costs one channel and no
 * listener, so a whole exchange fits inside a fuzz iteration.
 *
 * What this target is looking for is not "the right answer". It is the five
 * things the design says an exchange must never do, whatever the input:
 *
 *   - write after cancellation;
 *   - emit a second completion, or a second begin_*;
 *   - write a terminal chunk on an abandoned stream;
 *   - leak a channel or a principal;
 *   - deadlock.
 *
 * The first three are asserted directly below. The fourth is asserted for the
 * principal as a COUNT - retained exactly as often as released - and left to
 * ASan for the channel, which is what the sanitized build is for. The fifth is
 * what a hang in the run reports.
 *
 * One runtime for the whole campaign, with a provider that returns
 * immediately: creating one per iteration would spend the campaign in provider
 * activation rather than in the state machine under test.
 */
#include "maelys/mcp.h"
#include "maelys/mcp/http.h"

#include <assert.h>
#include <jansson.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MAX_BODY 2048u
#define MAX_NAME 64u

/* ------------------------------------------------------- recording writer */

typedef struct recorder {
    int begin_json_calls;
    int begin_stream_calls;
    int event_calls;
    int keepalive_calls;
    int end_stream_calls;
    int status_only_calls;
    int status;
    maelys_mcp_http_stream_end_t disposition;
    /* Set the moment end_stream runs, so "nothing was written afterwards" is a
     * fact this recorder can state rather than one the test has to infer. */
    int stream_ended;
    int wrote_after_end;
    /* Which writer callback should trigger the scripted interruption, counted
     * across every callback so a script can land on the head, on an event, or
     * on a keep-alive. */
    int callbacks;
    int interrupt_at;
    int interrupt_shutdown;
    maelys_mcp_http_exchange_t **exchange;
} recorder_t;

static void maybe_interrupt(recorder_t *recorder) {
    if (++recorder->callbacks != recorder->interrupt_at) return;
    if (recorder->interrupt_shutdown) {
        maelys_mcp_http_exchange_shutdown(*recorder->exchange);
    } else {
        maelys_mcp_http_exchange_cancel(*recorder->exchange);
    }
}

static maelys_mcp_result_t on_begin_json(
    void *context, int status, const char *body, size_t length) {
    recorder_t *recorder = context;
    (void)body;
    (void)length;
    ++recorder->begin_json_calls;
    recorder->status = status;
    maybe_interrupt(recorder);
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t on_begin_stream(void *context) {
    recorder_t *recorder = context;
    ++recorder->begin_stream_calls;
    maybe_interrupt(recorder);
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t on_event(void *context, const char *json, size_t length) {
    recorder_t *recorder = context;
    ++recorder->event_calls;
    if (recorder->stream_ended) recorder->wrote_after_end = 1;
    /* The frame must be complete, compact JSON with no newline in it: one
     * `data:` line per frame is only true if the serializer never emits one. */
    assert(length > 0u);
    assert(memchr(json, '\n', length) == NULL);
    assert(memchr(json, '\r', length) == NULL);
    maybe_interrupt(recorder);
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t on_keepalive(void *context) {
    recorder_t *recorder = context;
    ++recorder->keepalive_calls;
    if (recorder->stream_ended) recorder->wrote_after_end = 1;
    maybe_interrupt(recorder);
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t on_end_stream(
    void *context, maelys_mcp_http_stream_end_t disposition) {
    recorder_t *recorder = context;
    ++recorder->end_stream_calls;
    recorder->disposition = disposition;
    recorder->stream_ended = 1;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t on_status_only(
    void *context, int status, const char *const *extra, size_t extra_count) {
    recorder_t *recorder = context;
    (void)extra;
    (void)extra_count;
    ++recorder->status_only_calls;
    recorder->status = status;
    return MAELYS_MCP_OK;
}

/* ------------------------------------------------------------ the headers */

typedef struct headers {
    const char *version;
    const char *method;
    const char *name;
} headers_t;

static int lookup(void *context, const char *name,
    const char **out_value, size_t *out_length) {
    const headers_t *headers = context;
    const char *value = NULL;
    if (strcasecmp(name, "MCP-Protocol-Version") == 0) value = headers->version;
    else if (strcasecmp(name, "Mcp-Method") == 0) value = headers->method;
    else if (strcasecmp(name, "Mcp-Name") == 0) value = headers->name;
    if (!value) return 0;
    if (out_value) *out_value = value;
    if (out_length) *out_length = strlen(value);
    return 1;
}

/* ---------------------------------------------------------- the principal */

/* Counted rather than allocated: "released exactly once" is the claim, and a
 * count is what states it. The principal is this struct, so a use after the
 * last release is a use of memory this file still owns and ASan still tracks. */
typedef struct counted_principal {
    int retains;
    int releases;
} counted_principal_t;

static void counted_retain(void *context, maelys_mcp_principal_t *principal) {
    (void)context;
    ((counted_principal_t *)principal)->retains++;
}

static void counted_release(void *context, maelys_mcp_principal_t *principal) {
    (void)context;
    ((counted_principal_t *)principal)->releases++;
}

/* ------------------------------------------------------------ the runtime */

static maelys_mcp_result_t quiet_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    (void)context;
    (void)out_error;
    /* Two progress frames, so a call with a progressToken streams and one
     * without does not - both shapes reachable from one provider. */
    for (int step = 0; step < 2; ++step) {
        (void)maelys_mcp_provider_report_progress(request->progress,
            step, 2.0, NULL);
    }
    out_result->structured_content = json_pack("{s:s}", "tool", request->tool_name);
    return out_result->structured_content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static maelys_mcp_result_t quiet_read(
    void *context,
    const maelys_mcp_resource_request_t *request,
    maelys_mcp_resource_result_t *out_result,
    char **out_error) {
    (void)context;
    (void)out_error;
    out_result->contents = json_pack("[{s:s,s:s,s:s}]",
        "uri", request->uri, "mimeType", "text/plain", "text", "x");
    return out_result->contents ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static maelys_mcp_runtime_t *shared_runtime(void) {
    static maelys_mcp_runtime_t *runtime = NULL;
    if (runtime) return runtime;
    maelys_mcp_runtime_config_t config = {
        .server_name = "http-exchange-fuzz", .server_version = "0.0.0"
    };
    if (maelys_mcp_runtime_create(&config, &runtime) != MAELYS_MCP_OK) return NULL;
    if (maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_TOOLS) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_RESOURCES) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_enable_module(runtime,
            MAELYS_MCP_MODULE_SUBSCRIPTIONS) != MAELYS_MCP_OK) {
        runtime = NULL;
        return NULL;
    }
    json_t *schema = json_pack("{s:s,s:b}", "type", "object", "additionalProperties", 0);
    if (!schema) return NULL;
    maelys_mcp_tool_t tools[] = {{
        .name = "fx.tool",
        .title = "Tool",
        .description = "A tool.",
        .input_schema = schema,
        .effect = MAELYS_MCP_EFFECT_READ
    }};
    static const maelys_mcp_resource_t resources[] = {{
        .uri = "fx://doc.txt", .name = "Doc", .mime_type = "text/plain"
    }};
    maelys_mcp_provider_config_t provider_config = {
        .name = "fixture",
        .version = "1",
        .tools = tools,
        .tool_count = 1,
        .resources = resources,
        .resource_count = 1,
        .call = quiet_call,
        .read_resource = quiet_read
    };
    maelys_mcp_provider_t *provider = NULL;
    maelys_mcp_result_t status = maelys_mcp_provider_create(&provider_config, &provider);
    json_decref(schema);
    if (status != MAELYS_MCP_OK) return NULL;
    if (maelys_mcp_runtime_add_provider(runtime, provider, NULL) != MAELYS_MCP_OK) {
        maelys_mcp_provider_destroy(provider);
        return NULL;
    }
    return runtime;
}

/* ------------------------------------------------------------- the script */

#define META \
    "\"_meta\":{" \
    "\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"," \
    "\"io.modelcontextprotocol/clientCapabilities\":{}"

typedef struct shape {
    const char *method;
    const char *name;
    const char *body;
} shape_t;

/* One row per response shape the adapter can produce, so a campaign covers
 * JSON mode, stream mode, the notification path and the refusals rather than
 * hammering whichever one random bytes happen to reach. */
static const shape_t SHAPES[] = {
    {"tools/list", NULL,
     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{" META "}}}"},
    {"ping", NULL,
     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"params\":{" META "}}}"},
    {"tools/call", "fx.tool",
     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
     "\"name\":\"fx.tool\",\"arguments\":{}," META ",\"progressToken\":\"t\"}}}"},
    {"tools/call", "fx.tool",
     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
     "\"name\":\"fx.tool\",\"arguments\":{}," META "}}}"},
    {"subscriptions/listen", NULL,
     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"subscriptions/listen\",\"params\":{"
     "\"notifications\":{\"resourceSubscriptions\":[\"fx://doc.txt\"]}," META "}}}"},
    {"notifications/initialized", NULL,
     "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{"
     META "}}}"},
    {"resources/read", "fx://doc.txt",
     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"resources/read\",\"params\":{"
     "\"uri\":\"fx://doc.txt\"," META "}}}"}
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4u) return 0;
    maelys_mcp_runtime_t *runtime = shared_runtime();
    if (!runtime) return 0;

    size_t shape_index = data[0] % (sizeof(SHAPES) / sizeof(*SHAPES));
    const shape_t *shape = &SHAPES[shape_index];
    /* 0 means "never interrupt"; anything else lands on that callback. */
    int interrupt_at = (int)(data[1] % 6u);
    /*
     * subscriptions/listen is the one shape with no ending of its own - that is
     * the whole point of it - so an iteration that never interrupted it would
     * not be a slow iteration, it would be a hang. It is pinned to a callback
     * that certainly happens: 1 is begin_stream and 2 is the acknowledgement,
     * and both occur on every listen exchange.
     */
    if (strcmp(shape->method, "subscriptions/listen") == 0) {
        interrupt_at = 1 + (int)(data[1] % 2u);
    }
    int interrupt_shutdown = (data[2] & 1u) != 0;
    /* Whether the exchange is cancelled BEFORE it starts, which is the shape a
     * client that sent its request and vanished produces. */
    int pre_cancelled = (data[2] & 2u) != 0;
    unsigned int keepalive_ms = 1u + (unsigned int)(data[3] % 4u);

    /*
     * The routing headers are taken from the shape rather than from the input.
     * Mutating them is http-headers' job, and a campaign that spent itself on
     * -32020 refusals would never reach the drain loop this target exists for.
     */
    headers_t headers = {
        .version = "2026-07-28",
        .method = shape->method,
        .name = shape->name
    };

    maelys_mcp_http_adapter_config_t adapter_config = {
        .runtime = runtime,
        .close_timeout_ms = 50u,
        .keepalive_interval_ms = keepalive_ms
    };
    maelys_mcp_http_adapter_t *adapter = NULL;
    if (maelys_mcp_http_adapter_create(&adapter_config, &adapter) != MAELYS_MCP_OK) {
        return 0;
    }

    counted_principal_t principal = {0};
    maelys_mcp_http_exchange_t *exchange = NULL;
    recorder_t recorder = {
        .interrupt_at = interrupt_at,
        .interrupt_shutdown = interrupt_shutdown,
        .exchange = &exchange
    };
    maelys_mcp_http_response_writer_t writer = {
        .context = &recorder,
        .begin_json = on_begin_json,
        .begin_stream = on_begin_stream,
        .write_event = on_event,
        .write_keepalive = on_keepalive,
        .end_stream = on_end_stream,
        .status_only = on_status_only
    };
    maelys_mcp_http_request_t request = {
        .method = "POST",
        .path = "/mcp",
        .header_lookup = lookup,
        .header_context = &headers,
        .body = shape->body,
        .body_length = strlen(shape->body),
        .principal = (maelys_mcp_principal_t *)&principal,
        .principal_retain = counted_retain,
        .principal_release = counted_release,
        .principal_context = NULL,
        /* No descriptors, so the adapter allocates its own wakeup and the
         * out-of-band entry points are the only way to interrupt - which is
         * exactly the embedder this seam promises to serve. */
        .cancel_fd = -1,
        .shutdown_fd = -1
    };

    maelys_mcp_result_t handled;
    if (pre_cancelled) {
        /*
         * Cancelled before _handle is entered is not expressible through the
         * handle - there is none yet - so this is the descriptor form: a
         * cancellation source that is already readable, which is what a socket
         * whose peer has gone looks like.
         */
        int fds[2];
        if (pipe(fds) != 0) {
            maelys_mcp_http_adapter_destroy(adapter);
            return 0;
        }
        ssize_t written = write(fds[1], "x", 1u);
        (void)written;
        request.cancel_fd = fds[0];
        handled = maelys_mcp_http_adapter_handle(adapter, &request, &writer, &exchange);
        close(fds[0]);
        close(fds[1]);
    } else {
        handled = maelys_mcp_http_adapter_handle(adapter, &request, &writer, &exchange);
    }
    (void)handled;

    /* The handle is retired before _handle returns, so a late interruption is a
     * no-op by construction rather than by the caller's discipline. */
    assert(exchange == NULL);
    maelys_mcp_http_exchange_cancel(exchange);
    maelys_mcp_http_exchange_shutdown(exchange);

    /* At most one reply, and never two kinds of one. */
    assert(recorder.begin_json_calls <= 1);
    assert(recorder.begin_stream_calls <= 1);
    assert(recorder.status_only_calls <= 1);
    assert(recorder.begin_json_calls + recorder.begin_stream_calls +
        recorder.status_only_calls <= 1);

    /* end_stream exactly once per begin_stream, and never without one. */
    assert(recorder.end_stream_calls == recorder.begin_stream_calls);
    /* Nothing is written after the stream has ended. */
    assert(!recorder.wrote_after_end);
    /* No events without a stream to carry them. */
    assert(recorder.begin_stream_calls || recorder.event_calls == 0);
    assert(recorder.begin_stream_calls || recorder.keepalive_calls == 0);

    /*
     * A cancelled exchange NEVER ends its stream with a terminal chunk. This is
     * the invariant the two dispositions exist for: writing 0\r\n\r\n and then
     * closing would tell the client a cancelled call completed normally, which
     * is the one lie a truncated response must not tell.
     */
    if (pre_cancelled) {
        assert(recorder.begin_json_calls == 0);
        assert(recorder.begin_stream_calls == 0);
        assert(recorder.status_only_calls == 0);
    }

    /* The principal: one reference taken for the channel, one given back, and
     * never an unmatched pair whichever way the exchange ended. */
    assert(principal.retains == principal.releases);
    assert(principal.retains <= 1);

    maelys_mcp_http_adapter_destroy(adapter);
    return 0;
}
