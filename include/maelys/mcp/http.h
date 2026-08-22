#pragma once

#include "maelys/mcp/channel.h"
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
 * H3 STATUS. This header is the whole contract, and the adapter behind it now
 * SERVES MCP: it creates a channel per POST bound to the caller's principal and
 * to MAELYS_MCP_ERA_MODERN, accepts the frame, drains the channel's outbox,
 * chooses application/json or text/event-stream from the first frame, answers a
 * notification 202, and runs the cancellation and shutdown chains. The 503
 * placeholder H1 and H2 answered from is gone, and so is the enum that selected
 * it.
 *
 * What is NOT proven here is the conformance claim: H4 points the official
 * runner at the real binary with no adapter in the path, and until it does,
 * docs/protocol-support.md says so.
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
    /*
     * Established by the server layer, and bound to this exchange's channel as
     * maelys_mcp_channel_config_t::context so that middleware and modules see
     * the caller the runtime is acting for.
     *
     * BORROWED for the call. The server layer keeps owning the reference it
     * authenticated and releases it on its own schedule; the adapter takes a
     * SECOND reference through principal_retain below and gives that one back
     * through principal_release, at the moment the channel stops being
     * reachable. Two references rather than a transfer, because a channel that
     * misses its close deadline is destroyed detached and its context outlives
     * _handle - so "release when _handle returns" and "release when the channel
     * is freed" are different instants and each needs its own reference.
     */
    maelys_mcp_principal_t *principal;
    /*
     * The authenticator's refcount pair for `principal`, and its context.
     *
     * Supply both or neither. With both, the adapter retains before
     * channel_create and installs a release that the runtime runs EXACTLY ONCE
     * at the real free - inline for a clean close, and on whichever worker
     * finishes last for a detached one. With neither, the channel carries the
     * principal with no destructor and the embedder must keep it alive until
     * maelys_mcp_runtime_destroy has returned, which is the only bound left to
     * it; a transport that can detach should always supply them.
     */
    void (*principal_retain)(void *context, maelys_mcp_principal_t *principal);
    void (*principal_release)(void *context, maelys_mcp_principal_t *principal);
    void *principal_context;
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
    /*
     * The same shape, for the other way an exchange can end early: a descriptor
     * that becomes readable when this exchange should finish GRACEFULLY. It is
     * a different signal from cancel_fd and not a synonym - cancellation ends
     * the stream truncated, with no terminal chunk, because the peer is gone,
     * while shutdown completes every surviving subscription with
     * `resultType: "complete"`, writes those frames, and closes the body
     * properly, because the peer is still there and is owed an ending.
     *
     * One descriptor may be shared by every in-flight exchange, which is what a
     * server-wide "stop" is: the adapter only ever polls it.
     *
     * -1 means the embedder uses maelys_mcp_http_exchange_shutdown instead.
     */
    int shutdown_fd;
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
 * What every channel this adapter creates is configured with, plus the two
 * durations the drain loop needs. Zero means "the runtime's own default" for
 * each channel field, exactly as maelys_mcp_channel_config_t already defines
 * it, so a zero-initialized config is a working one.
 */
typedef struct maelys_mcp_http_adapter_config {
    /* Required. Every exchange creates one channel on it and destroys that
     * channel inside the request. */
    maelys_mcp_runtime_t *runtime;
    size_t max_messages;
    size_t max_bytes;
    size_t response_burst;
    unsigned int admission_timeout_ms;
    /* The bound on the close phase of every exchange, cancelled or not. A close
     * that misses it detaches rather than waits, so this is what stops a wedged
     * provider from holding a connection slot. 0 selects the channel default. */
    unsigned int close_timeout_ms;
    /*
     * How long the drain loop waits with nothing to write before emitting an
     * SSE keep-alive comment. 0 selects MAELYS_MCP_HTTP_KEEPALIVE_INTERVAL_MS.
     * It is a keep-alive interval and NOT a cancellation poll: cancellation
     * arrives as descriptor readiness, so shortening this buys no latency and
     * lengthening it costs none.
     */
    unsigned int keepalive_interval_ms;
} maelys_mcp_http_adapter_config_t;

/* The design's limits table: "SSE keep-alive | 15 s". */
#define MAELYS_MCP_HTTP_KEEPALIVE_INTERVAL_MS 15000u
/* The same 5 s maelys_mcp_channel_create applies to a zero close_timeout_ms.
 * Named here because the adapter resolves the zero itself, and two places
 * silently agreeing on a number is how they stop agreeing. */
#define MAELYS_MCP_HTTP_CLOSE_TIMEOUT_MS 5000u

maelys_mcp_result_t maelys_mcp_http_adapter_create(
    const maelys_mcp_http_adapter_config_t *config,
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
 *
 * WAKING THE DRAIN. An exchange that supplied cancel_fd or shutdown_fd is woken
 * by the embedder making that descriptor readable, which is the whole reason
 * they exist; this call only records the disposition. An exchange that supplied
 * NEITHER descriptor is woken by this call itself, through a wakeup pipe the
 * adapter allocates for exactly that case - so an embedder with no descriptor
 * of its own still gets prompt cancellation rather than one bounded by the
 * keep-alive interval.
 */
void maelys_mcp_http_exchange_cancel(maelys_mcp_http_exchange_t *exchange);

/*
 * The graceful sibling, with the same threading and lifetime rules: callable
 * from any thread while _handle is running, idempotent, a no-op afterwards.
 *
 * It asks the exchange to END, not to STOP. Every subscription still open on
 * this exchange's channel is completed with `resultType: "complete"`, those
 * frames are written, and the stream is terminated properly - which is what
 * server shutdown owes a client that is still connected and still reading. A
 * cancellation, by contrast, writes nothing further and leaves the body
 * truncated on purpose.
 *
 * A cancellation already recorded wins: a peer that is gone cannot be given a
 * graceful ending, and pretending otherwise would write to a dead socket.
 */
void maelys_mcp_http_exchange_shutdown(maelys_mcp_http_exchange_t *exchange);

#ifdef __cplusplus
}
#endif
