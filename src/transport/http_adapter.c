/*
 * The HTTP <-> MCP adapter, H1 shape.
 *
 * H1 builds both sides of the seam in include/maelys/mcp/http.h because they
 * are one contract, and stops there: "the adapter answers from a table: it
 * validates nothing and dispatches nothing yet"
 * (docs/http-transport-design.md, H1). Header/body validation and -32020 are
 * H2; channel_create, channel_accept, mode selection on the first frame, SSE
 * framing and the cancellation chain are H3.
 *
 * What is real here and must stay real is the shape: no descriptor is touched,
 * no socket header is included, the exchange handle is published before any
 * work and retired before _handle returns, and the writer is the only way a
 * byte leaves. The file compiles into libmaelys_mcp.a and
 * scripts/audit_boundaries.sh checks that it brought no listener primitive with
 * it.
 */
#include "maelys/mcp/http.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/*
 * The one placeholder body. 503 with -32600 and no id is the status table's
 * shape for "the runtime cannot serve this request", which is exactly true
 * here: the endpoint parses, routes and authenticates, and there is no dispatch
 * behind it until H3. Answering 200 with a synthetic result would be the one
 * thing a placeholder must not do.
 */
static const char PLACEHOLDER_JSON[] =
    "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,"
    "\"message\":\"The HTTP transport does not serve MCP yet.\"}}";

struct maelys_mcp_http_adapter {
    maelys_mcp_http_placeholder_t placeholder;
};

/*
 * The handle a canceller names. `cancelled` is written by
 * maelys_mcp_http_exchange_cancel from any thread and read by _handle, both
 * under `mutex`; `live` is what makes a late cancel a no-op by construction
 * rather than by the caller's discipline.
 */
struct maelys_mcp_http_exchange {
    pthread_mutex_t mutex;
    int live;
    int cancelled;
};

maelys_mcp_result_t maelys_mcp_http_adapter_create(
    maelys_mcp_http_placeholder_t placeholder,
    maelys_mcp_http_adapter_t **out_adapter) {
    if (!out_adapter) return MAELYS_MCP_ERR_ARGUMENT;
    if (placeholder != MAELYS_MCP_HTTP_PLACEHOLDER_JSON &&
        placeholder != MAELYS_MCP_HTTP_PLACEHOLDER_STREAM) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    maelys_mcp_http_adapter_t *adapter = calloc(1u, sizeof(*adapter));
    if (!adapter) return MAELYS_MCP_ERR_MEMORY;
    adapter->placeholder = placeholder;
    *out_adapter = adapter;
    return MAELYS_MCP_OK;
}

void maelys_mcp_http_adapter_destroy(maelys_mcp_http_adapter_t *adapter) {
    free(adapter);
}

void maelys_mcp_http_exchange_cancel(maelys_mcp_http_exchange_t *exchange) {
    if (!exchange) return;
    pthread_mutex_lock(&exchange->mutex);
    if (exchange->live) exchange->cancelled = 1;
    pthread_mutex_unlock(&exchange->mutex);
}

static int exchange_cancelled(maelys_mcp_http_exchange_t *exchange) {
    pthread_mutex_lock(&exchange->mutex);
    int cancelled = exchange->cancelled;
    pthread_mutex_unlock(&exchange->mutex);
    return cancelled;
}

/*
 * Retiring is what makes maelys_mcp_http_exchange_cancel safe after _handle
 * returns: the handle stops being reachable from the adapter, so a canceller
 * racing the return either got the lock first (and set a flag nobody reads
 * again) or finds `live` clear.
 */
static void retire_exchange(
    maelys_mcp_http_exchange_t *exchange,
    maelys_mcp_http_exchange_t **out_exchange) {
    if (out_exchange) *out_exchange = NULL;
    pthread_mutex_lock(&exchange->mutex);
    exchange->live = 0;
    pthread_mutex_unlock(&exchange->mutex);
    pthread_mutex_destroy(&exchange->mutex);
    free(exchange);
}

maelys_mcp_result_t maelys_mcp_http_adapter_handle(
    maelys_mcp_http_adapter_t *adapter,
    const maelys_mcp_http_request_t *request,
    const maelys_mcp_http_response_writer_t *writer,
    maelys_mcp_http_exchange_t **out_exchange) {
    if (out_exchange) *out_exchange = NULL;
    if (!adapter || !request || !writer) return MAELYS_MCP_ERR_ARGUMENT;
    if (!writer->begin_json || !writer->begin_stream || !writer->write_event ||
        !writer->end_stream || !writer->status_only) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    maelys_mcp_http_exchange_t *exchange = calloc(1u, sizeof(*exchange));
    if (!exchange) return MAELYS_MCP_ERR_MEMORY;
    if (pthread_mutex_init(&exchange->mutex, NULL) != 0) {
        free(exchange);
        return MAELYS_MCP_ERR_MEMORY;
    }
    exchange->live = 1;
    /* Published before any work, which is the whole reason the argument
     * exists - an out-of-band canceller has to have something to name before
     * the thing it wants to cancel has started. */
    if (out_exchange) *out_exchange = exchange;

    maelys_mcp_result_t status;
    if (adapter->placeholder == MAELYS_MCP_HTTP_PLACEHOLDER_STREAM) {
        status = writer->begin_stream(writer->context);
        if (status == MAELYS_MCP_OK) {
            /* The disposition, not a second entry point. An abandoned stream
             * ends without its terminal chunk; a completed one ends with it. */
            maelys_mcp_http_stream_end_t disposition = exchange_cancelled(exchange)
                ? MAELYS_MCP_HTTP_STREAM_ABORTED
                : MAELYS_MCP_HTTP_STREAM_COMPLETE;
            status = writer->end_stream(writer->context, disposition);
        }
    } else {
        status = writer->begin_json(writer->context, 503,
            PLACEHOLDER_JSON, sizeof(PLACEHOLDER_JSON) - 1u);
    }
    retire_exchange(exchange, out_exchange);
    return status;
}
