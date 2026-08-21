#pragma once

#include "maelys/mcp/error.h"
#include "maelys/mcp/http.h"
#include "maelys/mcp/principal.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The standalone HTTP/1.1 server layer.
 *
 * It lives in host/ and not in the library, which is the conclusion
 * docs/middleware-design.md:560 already reached for configuration - the library
 * keeps mechanism, the host keeps policy - and it is what keeps libmaelys_mcp
 * free of listen(2). scripts/audit_boundaries.sh enforces that mechanically
 * rather than trusting it.
 *
 * Everything the wire touches is here: accept, the connection lifetime, the
 * strict parse, Host and Origin validation, the framing and media rules,
 * routing, authentication, and the decision about whether the connection is
 * reused. What it hands the adapter is a parsed, routed, authenticated
 * maelys_mcp_http_request_t and nothing that would let the adapter tell a FIN
 * from a pipelined byte.
 *
 * H1 STATUS. The adapter behind this answers from a table and dispatches
 * nothing; docs/protocol-support.md stays honest about that until H2 and H3.
 */

/* Every duration and count is from the design's limits table. */
#define MAELYS_HTTP_HEADER_TIMEOUT_MS 5000u
#define MAELYS_HTTP_BODY_TIMEOUT_MS 30000u
#define MAELYS_HTTP_IDLE_TIMEOUT_MS 5000u
#define MAELYS_HTTP_MAX_CONNECTIONS 128u
/*
 * The bounded discard that makes a rejection reach the caller rather than
 * being lost to a reset (docs/authenticated-principal-design.md, "Authentication
 * runs before the body"). The 8 KiB is that document's; it names no deadline
 * for it, only "a short one - not the body limit, and not the body deadline",
 * so this reuses the shortest deadline the design's own limits table names
 * rather than inventing a value.
 */
#define MAELYS_HTTP_REJECT_DISCARD_BYTES 8192u
#define MAELYS_HTTP_REJECT_DISCARD_TIMEOUT_MS MAELYS_HTTP_HEADER_TIMEOUT_MS

typedef struct maelys_http_server maelys_http_server_t;

typedef struct maelys_http_server_options {
    /* Loopback by default: NULL means "127.0.0.1". */
    const char *bind_address;
    /* 0 asks the kernel for a port, which maelys_http_server_port reports. */
    unsigned short port;
    /* The host's path, not the library's. NULL means "/mcp". */
    const char *endpoint_path;
    const char *const *allowed_origins;
    size_t allowed_origin_count;
    /* The same ceiling stdio applies, and the same one the channel's output
     * budget is derived from. 0 means MAELYS_MCP_DEFAULT_MAX_MESSAGE_BYTES. */
    size_t max_body_bytes;
    size_t max_connections;
    unsigned int header_timeout_ms;
    unsigned int body_timeout_ms;
    unsigned int idle_timeout_ms;
    unsigned int write_timeout_ms;
    /*
     * Required. A non-loopback bind additionally refuses to start with an
     * authenticator that says it is loopback-only, which is the mechanical form
     * of "binding any other address requires an explicit flag and refuses to
     * start without an authenticator other than loopback-trust".
     */
    const maelys_mcp_authenticator_t *authenticator;
    maelys_mcp_http_adapter_t *adapter;
} maelys_http_server_options_t;

maelys_mcp_result_t maelys_http_server_start(
    const maelys_http_server_options_t *options,
    maelys_http_server_t **out_server);

/* The bound port, which is the only way to learn it when options.port was 0. */
unsigned short maelys_http_server_port(const maelys_http_server_t *server);

/*
 * Stop accepting, then let every in-flight connection finish, then join. Phase
 * ordering is the whole content: the listener closes first so no new connection
 * can arrive, and only then is anything asked to end.
 */
maelys_mcp_result_t maelys_http_server_stop(maelys_http_server_t *server);

/*
 * S2 of the cancellation chain, exported because it is the one piece of that
 * chain H1 can test: POLLIN fires both for "the peer sent data" and for "the
 * peer sent FIN", and a server that treated the first as a disconnect would
 * abort live requests whenever a client pipelined. Returns 0 for FIN, 1 for
 * inbound bytes mid-exchange (a protocol error, because pipelining is never
 * permitted), and -1 for "neither yet".
 */
int maelys_http_peek_disambiguate(int fd);

#ifdef __cplusplus
}
#endif
