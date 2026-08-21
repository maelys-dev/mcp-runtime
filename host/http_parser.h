#pragma once

#include "maelys/mcp/error.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The HTTP/1.1 request-line and header parser for the standalone listener.
 *
 * Hand-rolled, incremental, and host-level rather than library-level, because
 * the server layer lives in host/ and libmaelys_mcp.a stays free of listen(2)
 * (docs/http-transport-design.md, "Layering"). The argument for hand-rolling is
 * not zero dependencies - the project already links jansson and uriparser - it
 * is that a reader which accepts one method and one framing is a few hundred
 * reviewable lines that can be exhaustively fuzzed, whereas a general HTTP
 * library is a large attack surface bought for features this endpoint refuses
 * on purpose.
 *
 * src/jsonrpc/core.c is the model copied here: the append/consume ring, the
 * CRLFCRLF terminator scan, the overflow-checked decimal parse and the
 * duplicate-header refusal. Its parse_frames is deliberately NOT reused - it is
 * an LSP Content-Length framer with no request line, no method and no header
 * table, and bending it into an HTTP parser would make one function serve two
 * protocols.
 *
 * Nothing here touches a socket. The caller feeds bytes and reads the verdict,
 * which is what lets fuzz/fuzz_http_request.c drive the whole state machine
 * with no listener.
 *
 * THE ONE INVARIANT. On MAELYS_HTTP_PARSE_COMPLETE the parser has established a
 * single unambiguous body length or it has refused. There is no path on which
 * two readers of the same bytes could disagree about where this request ends,
 * which is the property fuzz/fuzz_http_smuggling.c asserts directly.
 */

/* Every limit is from the design's table. None is invented here. */
#define MAELYS_HTTP_MAX_REQUEST_LINE_BYTES 8192u
#define MAELYS_HTTP_MAX_HEADER_BYTES 16384u
#define MAELYS_HTTP_MAX_HEADER_COUNT 64u

typedef enum maelys_http_parse_state {
    MAELYS_HTTP_PARSE_INCOMPLETE = 0,
    MAELYS_HTTP_PARSE_COMPLETE = 1,
    MAELYS_HTTP_PARSE_REJECTED = 2
} maelys_http_parse_state_t;

/*
 * Why a rejection enum rather than a status code: the status is the server
 * layer's mapping, and two rejections that share a status do not share a log
 * line. SMUGGLING and BAD_REQUEST are both 400 and must stay distinguishable -
 * Transfer-Encoding together with Content-Length is the single most exploited
 * desync primitive and deserves its own signal even though the blanket
 * Transfer-Encoding rule already covers it.
 */
typedef enum maelys_http_reject {
    MAELYS_HTTP_REJECT_NONE = 0,
    MAELYS_HTTP_REJECT_BAD_REQUEST,        /* 400 + close */
    MAELYS_HTTP_REJECT_SMUGGLING,          /* 400 + close, logged as smuggling */
    MAELYS_HTTP_REJECT_HEADERS_TOO_LARGE,  /* 431 + close */
    MAELYS_HTTP_REJECT_VERSION             /* 505 + close */
} maelys_http_reject_t;

typedef struct maelys_http_header {
    size_t name_offset;
    size_t name_length;
    size_t value_offset;
    size_t value_length;
} maelys_http_header_t;

typedef struct maelys_http_parser {
    char *buffer;
    size_t length;
    size_t capacity;
    /* How far the bare-LF / terminator scan has already looked, and where the
     * line it is inside began, both carried across feeds so that feeding one
     * byte at a time costs the same total work as feeding the whole block. */
    size_t scanned;
    size_t line_start;
    /* One past the request line's CRLF, once that line has ended; the header
     * block's own byte budget is measured from here. `have_header_start`
     * distinguishes "the request line ended at offset 0" from "not yet". */
    size_t header_start;
    int have_header_start;

    size_t max_request_line_bytes;
    size_t max_header_bytes;
    size_t max_header_count;

    maelys_http_parse_state_t state;
    maelys_http_reject_t reject;

    size_t method_offset;
    size_t method_length;
    size_t target_offset;
    size_t target_length;

    maelys_http_header_t headers[MAELYS_HTTP_MAX_HEADER_COUNT];
    size_t header_count;

    /* Framing, established once and never revised. */
    int has_content_length;
    size_t content_length;

    /* Bytes the request line, the headers and the terminating CRLFCRLF
     * occupied. Everything after this in the buffer is body-or-pipelined and
     * belongs to the caller. */
    size_t header_block_length;
} maelys_http_parser_t;

void maelys_http_parser_init(maelys_http_parser_t *parser);
void maelys_http_parser_clear(maelys_http_parser_t *parser);

/*
 * Feed bytes. Returns the state after the feed. Once COMPLETE or REJECTED the
 * verdict is final and further feeds change nothing: this parser never
 * resynchronizes, because a parser that tries to find the next request after a
 * framing error is the parser that gets smuggled through.
 */
maelys_http_parse_state_t maelys_http_parser_feed(
    maelys_http_parser_t *parser,
    const void *bytes,
    size_t length);

/* Bytes past the header block that the feed already buffered. On a request
 * with a body these are its first bytes; with no body outstanding they are a
 * pipelined request, which the server layer refuses. */
const char *maelys_http_parser_residue(
    const maelys_http_parser_t *parser,
    size_t *out_length);

/*
 * Case-insensitive header lookup. Returns 1 only when the name occurs EXACTLY
 * ONCE; a repeated header is reported absent rather than merged, which is what
 * makes "present once" the meaning of a hit for everything downstream.
 */
int maelys_http_parser_header(
    const maelys_http_parser_t *parser,
    const char *name,
    const char **out_value,
    size_t *out_length);

/* Views into the parser's own buffer, valid until the next feed or clear. */
const char *maelys_http_parser_method(
    const maelys_http_parser_t *parser, size_t *out_length);
const char *maelys_http_parser_target(
    const maelys_http_parser_t *parser, size_t *out_length);

/*
 * Host validation. `expect_loopback` is set when the listener is bound to a
 * loopback address, in which case the authority must itself be a loopback
 * authority. Returns 1 when the value is acceptable.
 */
int maelys_http_host_valid(const char *value, size_t length, int expect_loopback);

/*
 * Origin validation - the DNS-rebinding control, and the check that runs
 * earliest. An Origin present and not in the allowlist is refused; an absent
 * Origin is accepted only on a loopback bind. The allowlist is empty by
 * default, so on a non-loopback bind with no configured origins every
 * cross-origin request is refused.
 */
int maelys_http_origin_allowed(
    const char *value,
    size_t length,
    const char *const *allowed,
    size_t allowed_count,
    int bind_is_loopback);

/*
 * Request-target normalization, in one pass and only one. Resolves absolute
 * form against `host` (an absolute-form authority that does not match Host is
 * refused), refuses `*` and authority form, percent-decodes exactly once, and
 * refuses a NUL, a control byte, a malformed escape or a `..` segment. On
 * success the decoded path is written to `out` NUL-terminated and compared
 * literally by the caller: a target that needs more than this is not this
 * endpoint.
 */
int maelys_http_target_normalize(
    const char *target,
    size_t target_length,
    const char *host,
    size_t host_length,
    char *out,
    size_t out_capacity);

/* Content-Type must be application/json, optionally with a charset=utf-8
 * parameter and nothing else. */
int maelys_http_content_type_json(const char *value, size_t length);

/* Accept must list both application/json and text/event-stream. */
int maelys_http_accept_sufficient(const char *value, size_t length);

#ifdef __cplusplus
}
#endif
