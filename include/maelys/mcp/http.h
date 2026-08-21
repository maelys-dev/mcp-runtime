#pragma once

#include "maelys/mcp/error.h"
#include "maelys/mcp/principal.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The HTTP <-> MCP adapter seam (docs/http-transport-design.md, "Layering").
 *
 * Three layers, and this header is the API between the first two. The server
 * layer - the listener, the parser, the connection lifetime - lives in host/
 * and never appears here. The adapter lives in the library and its inbound
 * contract is that it never touches a socket: there is no descriptor in
 * maelys_mcp_http_request_t other than an ABSTRACT cancellation source, and
 * nothing it receives would let it distinguish a FIN from a pipelined byte.
 * That distinction is HTTP-server knowledge and stays with the layer that owns
 * the pipelining rule which makes it matter.
 *
 * The split buys three things: the standalone binary owns routing and the path
 * (`/mcp` is the host's default, never the library's); an orchestrator that has
 * already terminated TLS reuses the adapter verbatim behind its own listener;
 * and the fuzz targets get a seam that is a function rather than a socket.
 *
 * H2 STATUS. This header is the whole contract. The adapter behind it now
 * validates the MCP routing headers against the body and refuses a
 * disagreement with -32020, and it still does not dispatch: see
 * maelys_mcp_http_adapter_create. channel_create, channel_accept, mode
 * selection on the first frame, SSE framing, 202 for notifications and the
 * cancellation chain are H3.
 */

/* One in-flight exchange. Opaque; created by the adapter, valid from the moment
 * _handle is entered until it returns. Carries the request and the channel, so
 * a canceller never has to name either. */
typedef struct maelys_mcp_http_exchange maelys_mcp_http_exchange_t;

typedef struct maelys_mcp_http_adapter maelys_mcp_http_adapter_t;

/*
 * Read-only access to the parsed header block. The embedder supplies the
 * lookup, because it already parsed the headers and the adapter must not assume
 * a representation. Returns 1 and fills *out_value / *out_length when present,
 * 0 when absent. `name` is ASCII and matched case-insensitively.
 *
 * A header the embedder saw more than once MUST be reported as absent. The
 * server layer refuses duplicates of every protocol header before the adapter
 * is reached, and a lookup that silently picked one of them would undo that -
 * the header/body validation the adapter performs is only sound if "present
 * once" is what a hit means.
 */
typedef int (*maelys_mcp_http_header_lookup_fn)(
    void *context,
    const char *name,
    const char **out_value,
    size_t *out_length);

typedef struct maelys_mcp_http_request {
    const char *method;              /* "POST"; the server layer rejects others */
    const char *path;                /* already routed; informational */
    /* The adapter reads MCP-Protocol-Version, Mcp-Method and Mcp-Name through
     * this and nothing else. It never asks for Authorization. */
    maelys_mcp_http_header_lookup_fn header_lookup;
    void *header_context;
    const void *body;
    size_t body_length;
    /* Established by the server layer. Borrowed for the call; the server layer
     * owns the retain/release pair (docs/authenticated-principal-design.md). */
    maelys_mcp_principal_t *principal;
    /*
     * An ABSTRACT cancellation source: a descriptor that becomes readable when
     * this exchange has been cancelled, and stays readable. What made it
     * readable is the embedder's business - a client FIN, an admin abort, a
     * deadline. The adapter adds it to its poll set and reads nothing from it.
     *
     * -1 means the embedder cancels out-of-band instead, by calling
     * maelys_mcp_http_exchange_cancel from another thread. An embedder that
     * offers neither gets no cancellation, and _handle says so in its result
     * rather than pretending otherwise.
     */
    int cancel_fd;
} maelys_mcp_http_request_t;

typedef enum maelys_mcp_http_stream_end {
    /* The exchange finished normally: write the terminal chunk. */
    MAELYS_MCP_HTTP_STREAM_COMPLETE = 0,
    /* The exchange was abandoned: do NOT write the terminal chunk; close the
     * connection so the peer sees a truncated body. A chunked body that ends
     * without its terminal chunk is exactly how HTTP/1.1 says "this response is
     * truncated"; writing 0\r\n\r\n and then closing would instead tell the
     * client the stream completed normally, which is the one lie a truncated
     * response must not tell. */
    MAELYS_MCP_HTTP_STREAM_ABORTED = 1
} maelys_mcp_http_stream_end_t;

typedef struct maelys_mcp_http_response_writer {
    void *context;
    /* Exactly one of begin_json / begin_stream is called, at most once. */
    maelys_mcp_result_t (*begin_json)(void *ctx, int status,
        const char *body, size_t length);
    maelys_mcp_result_t (*begin_stream)(void *ctx);
    /* One SSE event. Must flush before returning. */
    maelys_mcp_result_t (*write_event)(void *ctx, const char *json, size_t length);
    /* An SSE comment keep-alive. NULL is legal and means "no keep-alives". */
    maelys_mcp_result_t (*write_keepalive)(void *ctx);
    /* Ends a stream begun by begin_stream. Called exactly once per
     * begin_stream, with the disposition; what varies is what it writes. */
    maelys_mcp_result_t (*end_stream)(void *ctx,
        maelys_mcp_http_stream_end_t disposition);
    /* A status-only reply with no MCP body. */
    maelys_mcp_result_t (*status_only)(void *ctx, int status,
        const char *const *extra_headers, size_t extra_header_count);
} maelys_mcp_http_response_writer_t;

/*
 * The placeholder answer, unchanged by H2. The adapter now validates, but it
 * still "dispatches nothing yet" - "Still no dispatch" is H2's own boundary in
 * docs/http-transport-design.md - so this enum remains the table that selects
 * which canned answer a request that PASSED validation gets, and it is what
 * lets a recording writer drive both response modes with no socket involved.
 *
 * It exists only until H3 replaces the table with mode selection on the first
 * frame the channel produces, and disappears with it.
 */
typedef enum maelys_mcp_http_placeholder {
    /* begin_json with 503 and a -32600 body carrying no id: this endpoint
     * parses and authenticates, and does not serve MCP yet. */
    MAELYS_MCP_HTTP_PLACEHOLDER_JSON = 0,
    /* begin_stream, no events, end_stream(COMPLETE). */
    MAELYS_MCP_HTTP_PLACEHOLDER_STREAM = 1
} maelys_mcp_http_placeholder_t;

maelys_mcp_result_t maelys_mcp_http_adapter_create(
    maelys_mcp_http_placeholder_t placeholder,
    maelys_mcp_http_adapter_t **out_adapter);

void maelys_mcp_http_adapter_destroy(maelys_mcp_http_adapter_t *adapter);

maelys_mcp_result_t maelys_mcp_http_adapter_handle(
    maelys_mcp_http_adapter_t *adapter,
    const maelys_mcp_http_request_t *request,
    const maelys_mcp_http_response_writer_t *writer,
    /* Published before any dispatch, so an out-of-band canceller has something
     * to name. NULL when the caller does not intend to cancel. */
    maelys_mcp_http_exchange_t **out_exchange);

/*
 * Callable from any thread while _handle is running. Idempotent. After _handle
 * returns, the handle is dead and calling this is a no-op rather than a
 * use-after-free: the adapter retires it before returning.
 *
 * Cancellation names an exchange rather than a request on purpose. Naming the
 * request would make its *address* a concurrent identity, which is a race the
 * moment _handle returns and the caller reuses or frees that struct.
 */
void maelys_mcp_http_exchange_cancel(maelys_mcp_http_exchange_t *exchange);

#ifdef __cplusplus
}
#endif
