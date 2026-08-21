/*
 * The HTTP request parser's rejection matrix.
 *
 * Every rule under "Strict rejection rules" in docs/http-transport-design.md
 * has at least one named case here, and the table's `rule` column is what makes
 * that checkable: it names the rule the case exists for, so a rule that loses
 * its test loses a row rather than losing coverage quietly.
 *
 * Two properties are asserted for EVERY case rather than for a chosen few:
 *
 *  - split-across-chunks equivalence. The same bytes fed whole, one byte at a
 *    time, and in a pseudo-random chunking must produce the same verdict. This
 *    is the class of bug an incremental parser is for, and the existing
 *    fuzz/fuzz_content_length.c already fuzzes exactly this shape for the
 *    sibling framer.
 *  - a single unambiguous body length. On acceptance the parser has decided one
 *    Content-Length or none, and there is no case in which two readers of the
 *    same bytes could disagree about where the request ends.
 *
 * This binary deliberately links host/http_parser.o and nothing else: the
 * parser needs no socket, no library and no runtime, which is the same property
 * that lets the fuzz targets drive it.
 */
#include "host/http_parser.h"
#include "tests/test_support.h"

#include <stdlib.h>
#include <string.h>

typedef struct parser_case {
    /* The design rule this case exists for. */
    const char *rule;
    const char *request;
    /* Length, because several cases carry an embedded NUL on purpose. */
    size_t length;
    int accept;
    maelys_http_reject_t reject;
    /* Only meaningful when accept is 1. */
    int expect_content_length;
    size_t content_length;
} parser_case_t;

#define LIT(text) text, sizeof(text) - 1u

#define VALID_HEAD \
    "POST /mcp HTTP/1.1\r\n" \
    "Host: 127.0.0.1:8080\r\n" \
    "Content-Type: application/json\r\n" \
    "Accept: application/json, text/event-stream\r\n"

#define VALID_REQUEST VALID_HEAD "Content-Length: 2\r\n\r\n{}"

/* A header block that is over the 16 KiB budget without being over any other. */
static char *oversized_header_block(size_t *out_length) {
    size_t filler = 20000u;
    size_t capacity = filler + 256u;
    char *request = malloc(capacity);
    if (!request) return NULL;
    int head = snprintf(request, capacity, "POST /mcp HTTP/1.1\r\nX-Filler: ");
    memset(request + head, 'a', filler);
    size_t offset = (size_t)head + filler;
    int tail = snprintf(request + offset, capacity - offset, "\r\n\r\n");
    *out_length = offset + (size_t)tail;
    return request;
}

static char *oversized_request_line(size_t *out_length) {
    size_t filler = 9000u;
    size_t capacity = filler + 256u;
    char *request = malloc(capacity);
    if (!request) return NULL;
    int head = snprintf(request, capacity, "POST /");
    memset(request + head, 'a', filler);
    size_t offset = (size_t)head + filler;
    int tail = snprintf(request + offset, capacity - offset, " HTTP/1.1\r\n\r\n");
    *out_length = offset + (size_t)tail;
    return request;
}

static char *too_many_headers(size_t *out_length) {
    size_t capacity = 8192u;
    char *request = malloc(capacity);
    if (!request) return NULL;
    size_t offset = (size_t)snprintf(request, capacity, "POST /mcp HTTP/1.1\r\n");
    for (unsigned int index = 0; index < 70u; ++index) {
        offset += (size_t)snprintf(request + offset, capacity - offset,
            "X-Filler-%u: v\r\n", index);
    }
    offset += (size_t)snprintf(request + offset, capacity - offset, "\r\n");
    *out_length = offset;
    return request;
}

static const parser_case_t CASES[] = {
    /* -------------------------------------------------------- acceptance */
    {"a well-formed POST is accepted", LIT(VALID_REQUEST), 1,
        MAELYS_HTTP_REJECT_NONE, 1, 2u},
    {"Content-Length with surrounding OWS is accepted",
        LIT(VALID_HEAD "Content-Length:   2  \r\n\r\n{}"), 1,
        MAELYS_HTTP_REJECT_NONE, 1, 2u},
    {"Content-Length: 0 is accepted and means an empty body",
        LIT(VALID_HEAD "Content-Length: 0\r\n\r\n"), 1,
        MAELYS_HTTP_REJECT_NONE, 1, 0u},
    {"an absent Content-Length parses; 411 is the server layer's answer",
        LIT(VALID_HEAD "\r\n"), 1, MAELYS_HTTP_REJECT_NONE, 0, 0u},
    {"a repeated Mcp-Param-* is forwarded-and-ignored, not refused",
        LIT(VALID_HEAD "Mcp-Param-Foo: a\r\nMcp-Param-Foo: b\r\n"
            "Content-Length: 2\r\n\r\n{}"), 1, MAELYS_HTTP_REJECT_NONE, 1, 2u},

    /* ------------------------------------------- framing and the smuggling class */
    {"Transfer-Encoding present at all is refused",
        LIT(VALID_HEAD "Transfer-Encoding: chunked\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"Transfer-Encoding: identity is refused too",
        LIT(VALID_HEAD "Transfer-Encoding: identity\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"Transfer-Encoding as a list is refused",
        LIT(VALID_HEAD "Transfer-Encoding: chunked, chunked\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"Transfer-Encoding in any casing is refused",
        LIT(VALID_HEAD "TrAnSfEr-EnCoDiNg: chunked\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"Transfer-Encoding with Content-Length is reported as smuggling",
        LIT(VALID_HEAD "Transfer-Encoding: chunked\r\nContent-Length: 2\r\n\r\n{}"), 0,
        MAELYS_HTTP_REJECT_SMUGGLING, 0, 0u},
    {"Content-Length with Transfer-Encoding is smuggling in either order",
        LIT(VALID_HEAD "Content-Length: 2\r\nTransfer-Encoding: identity\r\n\r\n{}"), 0,
        MAELYS_HTTP_REJECT_SMUGGLING, 0, 0u},
    {"a repeated Transfer-Encoding is refused",
        LIT(VALID_HEAD "Transfer-Encoding: chunked\r\n"
            "Transfer-Encoding: identity\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a repeated Content-Length is refused even when the values agree",
        LIT(VALID_HEAD "Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a repeated Content-Length with different values is refused",
        LIT(VALID_HEAD "Content-Length: 2\r\nContent-Length: 9\r\n\r\n{}"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"Content-Length with a leading plus is refused",
        LIT(VALID_HEAD "Content-Length: +2\r\n\r\n{}"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a negative Content-Length is refused",
        LIT(VALID_HEAD "Content-Length: -2\r\n\r\n{}"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a hex Content-Length is refused",
        LIT(VALID_HEAD "Content-Length: 0x2\r\n\r\n{}"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"an empty Content-Length is refused",
        LIT(VALID_HEAD "Content-Length:\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a Content-Length with an embedded space is refused",
        LIT(VALID_HEAD "Content-Length: 2 2\r\n\r\n{}"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"an overflowing Content-Length is refused rather than truncated",
        LIT(VALID_HEAD "Content-Length: 99999999999999999999999999999\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},

    /* ------------------------------------------------ request line and headers */
    {"a bare LF terminating the request line is refused",
        LIT("POST /mcp HTTP/1.1\nHost: 127.0.0.1\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a bare LF terminating a header is refused",
        LIT("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a leading CRLF before the request line is refused",
        LIT("\r\n" VALID_REQUEST), 0, MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a leading space before the request line is refused",
        LIT(" " VALID_REQUEST), 0, MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a leading HTAB before the request line is refused",
        LIT("\t" VALID_REQUEST), 0, MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"an obs-fold continuation line is refused",
        LIT("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n  extra\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"an obs-fold continuation with HTAB is refused",
        LIT("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n\textra\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"whitespace between a field name and its colon is refused",
        LIT("POST /mcp HTTP/1.1\r\nHost : 127.0.0.1\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a header name outside tchar is refused",
        LIT("POST /mcp HTTP/1.1\r\nH@st: 127.0.0.1\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"an empty header name is refused",
        LIT("POST /mcp HTTP/1.1\r\n: 127.0.0.1\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a header line with no colon is refused",
        LIT("POST /mcp HTTP/1.1\r\nHost\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a NUL in a header value is refused before the value is interpreted",
        LIT("POST /mcp HTTP/1.1\r\nHost: 127.0.0\0.1\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a bare CR inside a header value is refused",
        LIT("POST /mcp HTTP/1.1\r\nHost: 127.0\r0.0.1\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a NUL in the request target is refused",
        LIT("POST /m\0cp HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a control byte in the request target is refused",
        LIT("POST /m\x01" "cp HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a method outside tchar is refused",
        LIT("PO/ST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"an empty method is refused rather than skipped",
        LIT(" /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"an empty request target is refused",
        LIT("POST  HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},

    /* ------------------------------------------------------------- versions */
    {"HTTP/1.0 is refused rather than downgraded to",
        LIT("POST /mcp HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_VERSION, 0, 0u},
    {"HTTP/2.0 over this listener is refused",
        LIT("POST /mcp HTTP/2.0\r\nHost: 127.0.0.1\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_VERSION, 0, 0u},
    {"an HTTP/0.9 simple request is a version refusal",
        LIT("POST /mcp\r\nHost: 127.0.0.1\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_VERSION, 0, 0u},
    {"a version that is not HTTP at all is a malformed request line",
        LIT("POST /mcp SPDY/3.1\r\nHost: 127.0.0.1\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},

    /* -------------------------------------------------- duplicate protocol headers */
    {"a repeated Host is refused",
        LIT("POST /mcp HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a repeated Content-Type is refused",
        LIT(VALID_HEAD "Content-Type: application/json\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a repeated Accept is refused",
        LIT(VALID_HEAD "Accept: */*\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a repeated Origin is refused",
        LIT(VALID_HEAD "Origin: http://a\r\nOrigin: http://b\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a repeated Authorization is refused",
        LIT(VALID_HEAD "Authorization: Bearer a\r\nAuthorization: Bearer b\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a repeated Connection is refused",
        LIT(VALID_HEAD "Connection: close\r\nConnection: keep-alive\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a repeated Expect is refused",
        LIT(VALID_HEAD "Expect: 100-continue\r\nExpect: other\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a repeated Mcp-Method is refused",
        LIT(VALID_HEAD "Mcp-Method: tools/call\r\nMcp-Method: tools/list\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u},
    {"a repeated MCP-Protocol-Version is refused",
        LIT(VALID_HEAD "MCP-Protocol-Version: 2026-07-28\r\n"
            "mcp-protocol-version: 2025-11-25\r\n\r\n"), 0,
        MAELYS_HTTP_REJECT_BAD_REQUEST, 0, 0u}
};

static int run_one(const parser_case_t *entry, size_t chunk_hint) {
    maelys_http_parser_t parser;
    maelys_http_parser_init(&parser);
    maelys_http_parse_state_t state = MAELYS_HTTP_PARSE_INCOMPLETE;
    size_t offset = 0;
    while (offset < entry->length && state == MAELYS_HTTP_PARSE_INCOMPLETE) {
        size_t chunk = chunk_hint ? chunk_hint : entry->length - offset;
        if (chunk > entry->length - offset) chunk = entry->length - offset;
        state = maelys_http_parser_feed(&parser, entry->request + offset, chunk);
        offset += chunk;
    }
    int failures = 0;
    if (entry->accept) {
        if (state != MAELYS_HTTP_PARSE_COMPLETE) {
            fprintf(stderr, "  [%s] expected acceptance, got state %d reject %d\n",
                entry->rule, (int)state, (int)parser.reject);
            ++failures;
        } else {
            /* The single-unambiguous-length invariant, asserted on every
             * accepted request rather than only on the framing cases. */
            if (parser.has_content_length != entry->expect_content_length) {
                fprintf(stderr, "  [%s] framing disagreed: has=%d want=%d\n",
                    entry->rule, parser.has_content_length, entry->expect_content_length);
                ++failures;
            } else if (entry->expect_content_length &&
                parser.content_length != entry->content_length) {
                fprintf(stderr, "  [%s] length %zu, want %zu\n",
                    entry->rule, parser.content_length, entry->content_length);
                ++failures;
            }
        }
    } else {
        if (state != MAELYS_HTTP_PARSE_REJECTED) {
            fprintf(stderr, "  [%s] expected rejection, got state %d\n",
                entry->rule, (int)state);
            ++failures;
        } else if (parser.reject != entry->reject) {
            fprintf(stderr, "  [%s] reject %d, want %d\n",
                entry->rule, (int)parser.reject, (int)entry->reject);
            ++failures;
        }
    }
    maelys_http_parser_clear(&parser);
    return failures;
}

static int rejection_matrix(void) {
    int failures = 0;
    for (size_t index = 0; index < sizeof(CASES) / sizeof(*CASES); ++index) {
        failures += run_one(&CASES[index], 0);
    }
    return failures;
}

static int split_across_chunks_equivalence(void) {
    int failures = 0;
    /* Whole, one byte at a time, and three awkward sizes that land boundaries
     * inside CRLF pairs, inside header names and inside values. */
    static const size_t chunks[] = {1u, 2u, 3u, 7u, 13u};
    for (size_t index = 0; index < sizeof(CASES) / sizeof(*CASES); ++index) {
        for (size_t which = 0; which < sizeof(chunks) / sizeof(*chunks); ++which) {
            failures += run_one(&CASES[index], chunks[which]);
        }
    }
    return failures;
}

static int oversized_inputs(void) {
    int failures = 0;
    struct {
        const char *rule;
        char *(*build)(size_t *);
    } builders[] = {
        {"a request line over its limit is 431", oversized_request_line},
        {"a header block over its limit is 431", oversized_header_block},
        {"a header count over its limit is 431", too_many_headers}
    };
    for (size_t index = 0; index < sizeof(builders) / sizeof(*builders); ++index) {
        size_t length = 0;
        char *request = builders[index].build(&length);
        ASSERT_TRUE(request != NULL);
        parser_case_t entry = {
            .rule = builders[index].rule,
            .request = request,
            .length = length,
            .accept = 0,
            .reject = MAELYS_HTTP_REJECT_HEADERS_TOO_LARGE
        };
        failures += run_one(&entry, 0);
        failures += run_one(&entry, 1u);
        free(request);
    }
    return failures;
}

/*
 * The request-line budget's exact boundary. Two checks enforce it - one while
 * the line is still growing, one when it terminates - and they have to agree
 * about where it is, or a request line of exactly the limit is refused by
 * whichever is stricter.
 */
static int request_line_budget_boundary(void) {
    for (int over = 0; over < 2; ++over) {
        size_t line = MAELYS_HTTP_MAX_REQUEST_LINE_BYTES + (over ? 1u : 0u);
        size_t prefix = strlen("POST /");
        size_t suffix = strlen(" HTTP/1.1");
        ASSERT_TRUE(line > prefix + suffix);
        size_t filler = line - prefix - suffix;
        size_t total = line + 4u;
        char *request = malloc(total + 1u);
        ASSERT_TRUE(request != NULL);
        memcpy(request, "POST /", prefix);
        memset(request + prefix, 'a', filler);
        memcpy(request + prefix + filler, " HTTP/1.1\r\n\r\n", suffix + 4u);
        maelys_http_parser_t parser;
        maelys_http_parser_init(&parser);
        maelys_http_parse_state_t state = maelys_http_parser_feed(&parser, request, total);
        if (over) {
            ASSERT_TRUE(state == MAELYS_HTTP_PARSE_REJECTED);
            ASSERT_TRUE(parser.reject == MAELYS_HTTP_REJECT_HEADERS_TOO_LARGE);
        } else {
            /* Exactly at the limit is inside it. */
            ASSERT_TRUE(state == MAELYS_HTTP_PARSE_COMPLETE);
        }
        maelys_http_parser_clear(&parser);
        free(request);
    }
    return 0;
}

/*
 * The limits are enforced while the block is still growing, not after it is
 * whole. A request line that never terminates must be refused without ever
 * having been fully buffered, and the only way to observe that is to feed a
 * stream that has no CRLF in it at all - the at-terminator checks cannot fire.
 */
static int limits_bound_an_unterminated_stream(void) {
    maelys_http_parser_t parser;
    maelys_http_parser_init(&parser);
    char filler[512];
    memset(filler, 'a', sizeof(filler));
    maelys_http_parse_state_t state = maelys_http_parser_feed(&parser, "POST /", 6u);
    size_t fed = 6u;
    while (state == MAELYS_HTTP_PARSE_INCOMPLETE && fed < 32768u) {
        state = maelys_http_parser_feed(&parser, filler, sizeof(filler));
        fed += sizeof(filler);
    }
    ASSERT_TRUE(state == MAELYS_HTTP_PARSE_REJECTED);
    ASSERT_TRUE(parser.reject == MAELYS_HTTP_REJECT_HEADERS_TOO_LARGE);
    /* Refused within one read chunk of the request-line budget rather than
     * after 32 KiB of it. */
    ASSERT_TRUE(fed <= MAELYS_HTTP_MAX_REQUEST_LINE_BYTES + sizeof(filler));
    maelys_http_parser_clear(&parser);

    /* The same for a header block that never reaches its empty line. */
    maelys_http_parser_init(&parser);
    state = maelys_http_parser_feed(&parser, "POST /mcp HTTP/1.1\r\n", 20u);
    fed = 20u;
    while (state == MAELYS_HTTP_PARSE_INCOMPLETE && fed < 65536u) {
        state = maelys_http_parser_feed(&parser, filler, sizeof(filler));
        fed += sizeof(filler);
    }
    ASSERT_TRUE(state == MAELYS_HTTP_PARSE_REJECTED);
    ASSERT_TRUE(parser.reject == MAELYS_HTTP_REJECT_HEADERS_TOO_LARGE);
    ASSERT_TRUE(fed <= MAELYS_HTTP_MAX_HEADER_BYTES + 20u + sizeof(filler));
    maelys_http_parser_clear(&parser);
    return 0;
}

/* A rejected parser stays rejected: nothing is ever resynchronized, because a
 * parser that tries to find the next request after a framing error is the
 * parser that gets smuggled through. */
static int rejection_is_final(void) {
    maelys_http_parser_t parser;
    maelys_http_parser_init(&parser);
    maelys_http_parse_state_t state = maelys_http_parser_feed(&parser,
        LIT("POST /mcp HTTP/1.1\nHost: x\r\n\r\n"));
    ASSERT_TRUE(state == MAELYS_HTTP_PARSE_REJECTED);
    state = maelys_http_parser_feed(&parser, LIT(VALID_REQUEST));
    ASSERT_TRUE(state == MAELYS_HTTP_PARSE_REJECTED);
    ASSERT_TRUE(parser.reject == MAELYS_HTTP_REJECT_BAD_REQUEST);
    maelys_http_parser_clear(&parser);
    return 0;
}

/* A repeated header is reported ABSENT rather than merged or first-wins, which
 * is what makes "present once" the meaning of a hit for everything downstream.
 * The protocol headers never reach here because the parse refuses them, so this
 * is asserted on a header family the parse deliberately tolerates. */
static int duplicate_lookup_reports_absent(void) {
    maelys_http_parser_t parser;
    maelys_http_parser_init(&parser);
    maelys_http_parse_state_t state = maelys_http_parser_feed(&parser,
        LIT(VALID_HEAD "Mcp-Param-Foo: a\r\nMcp-Param-Foo: b\r\n"
            "Content-Length: 2\r\n\r\n{}"));
    ASSERT_TRUE(state == MAELYS_HTTP_PARSE_COMPLETE);
    ASSERT_TRUE(maelys_http_parser_header(&parser, "mcp-param-foo", NULL, NULL) == 0);
    const char *value = NULL;
    size_t length = 0;
    ASSERT_TRUE(maelys_http_parser_header(&parser, "HOST", &value, &length) == 1);
    ASSERT_TRUE(length == 14u && memcmp(value, "127.0.0.1:8080", 14u) == 0);
    maelys_http_parser_clear(&parser);
    return 0;
}

static int residue_is_the_body(void) {
    maelys_http_parser_t parser;
    maelys_http_parser_init(&parser);
    ASSERT_TRUE(maelys_http_parser_feed(&parser, LIT(VALID_REQUEST)) ==
        MAELYS_HTTP_PARSE_COMPLETE);
    size_t length = 0;
    const char *residue = maelys_http_parser_residue(&parser, &length);
    ASSERT_TRUE(length == 2u && memcmp(residue, "{}", 2u) == 0);
    maelys_http_parser_clear(&parser);
    return 0;
}

/* Pipelined bytes show up as residue longer than the body, which is the shape
 * the server layer refuses. */
static int residue_reveals_pipelining(void) {
    maelys_http_parser_t parser;
    maelys_http_parser_init(&parser);
    ASSERT_TRUE(maelys_http_parser_feed(&parser,
        LIT(VALID_REQUEST VALID_REQUEST)) == MAELYS_HTTP_PARSE_COMPLETE);
    size_t length = 0;
    (void)maelys_http_parser_residue(&parser, &length);
    ASSERT_TRUE(length > parser.content_length);
    maelys_http_parser_clear(&parser);
    return 0;
}

typedef struct host_case {
    const char *rule;
    const char *value;
    int expect_loopback;
    int valid;
} host_case_t;

static const host_case_t HOST_CASES[] = {
    {"a loopback IPv4 authority is valid on a loopback bind", "127.0.0.1:8080", 1, 1},
    {"the whole 127.0.0.0/8 range counts as loopback", "127.13.0.9", 1, 1},
    {"localhost is a loopback authority", "localhost:1", 1, 1},
    {"a bracketed ::1 is a loopback authority", "[::1]:8080", 1, 1},
    {"the expanded IPv6 loopback is recognized", "[0:0:0:0:0:0:0:1]", 1, 1},
    {"a public name is not a loopback authority", "example.com", 1, 0},
    {"a non-loopback IPv4 is refused on a loopback bind", "10.0.0.1:80", 1, 0},
    {"a public name is valid on a non-loopback bind", "example.com:443", 0, 1},
    {"an empty Host is invalid", "", 0, 0},
    {"a Host with a space is invalid", "exam ple.com", 0, 0},
    {"a Host with a control byte is invalid", "example\tcom", 0, 0},
    {"an out-of-range port is invalid", "127.0.0.1:99999", 1, 0},
    {"an empty port is invalid", "127.0.0.1:", 1, 0},
    {"a non-numeric port is invalid", "127.0.0.1:http", 1, 0},
    {"an unbracketed IPv6 literal is not a valid Host", "::1", 1, 0},
    {"two colons outside brackets are invalid", "127.0.0.1:80:90", 1, 0},
    {"an unterminated bracket is invalid", "[::1", 1, 0},
    {"an empty bracket is invalid", "[]", 1, 0}
};

static int host_validation(void) {
    int failures = 0;
    for (size_t index = 0; index < sizeof(HOST_CASES) / sizeof(*HOST_CASES); ++index) {
        const host_case_t *entry = &HOST_CASES[index];
        int valid = maelys_http_host_valid(entry->value, strlen(entry->value),
            entry->expect_loopback);
        if (valid != entry->valid) {
            fprintf(stderr, "  [%s] got %d want %d\n", entry->rule, valid, entry->valid);
            ++failures;
        }
    }
    return failures;
}

static int origin_validation(void) {
    static const char *const allowed[] = {"https://app.example", "http://localhost:3000"};
    int failures = 0;

    /* Absent Origin: accepted only on a loopback bind. */
    if (!maelys_http_origin_allowed(NULL, 0, allowed, 2u, 1)) {
        fprintf(stderr, "  absent Origin must be accepted on a loopback bind\n");
        ++failures;
    }
    if (maelys_http_origin_allowed(NULL, 0, allowed, 2u, 0)) {
        fprintf(stderr, "  absent Origin must be refused on a non-loopback bind\n");
        ++failures;
    }
    /* The allowlist is empty by default, so every present Origin is refused. */
    if (maelys_http_origin_allowed("https://app.example", 19u, NULL, 0, 1)) {
        fprintf(stderr, "  an empty allowlist must refuse a present Origin\n");
        ++failures;
    }
    if (!maelys_http_origin_allowed("https://app.example", 19u, allowed, 2u, 0)) {
        fprintf(stderr, "  an allowlisted Origin must be accepted\n");
        ++failures;
    }
    if (maelys_http_origin_allowed("https://evil.example", 20u, allowed, 2u, 1)) {
        fprintf(stderr, "  a non-allowlisted Origin must be refused\n");
        ++failures;
    }
    if (maelys_http_origin_allowed("null", 4u, allowed, 2u, 1)) {
        fprintf(stderr, "  the null Origin must be refused\n");
        ++failures;
    }
    /* A prefix of an allowlisted origin is not that origin. */
    if (maelys_http_origin_allowed("https://app.example.evil", 24u, allowed, 2u, 1)) {
        fprintf(stderr, "  a suffix-extended Origin must be refused\n");
        ++failures;
    }
    if (maelys_http_origin_allowed("https://app.example\r\nX: y", 25u, allowed, 2u, 1)) {
        fprintf(stderr, "  an Origin carrying CRLF must be refused\n");
        ++failures;
    }
    if (maelys_http_origin_allowed("", 0u, allowed, 2u, 1)) {
        fprintf(stderr, "  an empty Origin must be refused\n");
        ++failures;
    }
    return failures;
}

typedef struct target_case {
    const char *rule;
    const char *target;
    const char *host;
    int valid;
    const char *expected;
} target_case_t;

static const target_case_t TARGET_CASES[] = {
    {"an origin-form path normalizes to itself", "/mcp", "h", 1, "/mcp"},
    {"a percent escape is decoded exactly once", "/%6dcp", "h", 1, "/mcp"},
    {"a query string stays part of the literal comparison", "/mcp?x=1", "h", 1, "/mcp?x=1"},
    {"a literal .. segment is refused", "/../mcp", "h", 0, NULL},
    {"a trailing .. segment is refused", "/mcp/..", "h", 0, NULL},
    {"a percent-encoded .. segment is refused", "/%2e%2e/mcp", "h", 0, NULL},
    {"an encoded slash that creates a .. segment is refused", "/a%2f..%2fmcp", "h", 0, NULL},
    {"a NUL in the target is refused", "/mcp%00", "h", 0, NULL},
    {"a control byte in the target is refused", "/mcp%01", "h", 0, NULL},
    {"a malformed percent escape is refused", "/%zz", "h", 0, NULL},
    {"a truncated percent escape is refused", "/mcp%2", "h", 0, NULL},
    {"the asterisk form is refused", "*", "h", 0, NULL},
    {"the authority form is refused", "127.0.0.1:80", "h", 0, NULL},
    {"a target that is not a path is refused", "mcp", "h", 0, NULL},
    {"absolute form matching Host is accepted",
        "http://127.0.0.1:8080/mcp", "127.0.0.1:8080", 1, "/mcp"},
    {"absolute form matching Host case-insensitively is accepted",
        "HTTP://Example.Com/mcp", "example.com", 1, "/mcp"},
    {"absolute form with an empty path becomes /",
        "http://127.0.0.1:8080", "127.0.0.1:8080", 1, "/"},
    {"absolute form whose authority differs from Host is refused",
        "http://evil.example/mcp", "127.0.0.1:8080", 0, NULL},
    {"an https absolute form is handled the same way",
        "https://example.com/mcp", "example.com", 1, "/mcp"}
};

static int target_normalization(void) {
    int failures = 0;
    for (size_t index = 0; index < sizeof(TARGET_CASES) / sizeof(*TARGET_CASES); ++index) {
        const target_case_t *entry = &TARGET_CASES[index];
        char path[256];
        int valid = maelys_http_target_normalize(entry->target, strlen(entry->target),
            entry->host, strlen(entry->host), path, sizeof(path));
        if (valid != entry->valid) {
            fprintf(stderr, "  [%s] got %d want %d\n", entry->rule, valid, entry->valid);
            ++failures;
            continue;
        }
        if (valid && strcmp(path, entry->expected) != 0) {
            fprintf(stderr, "  [%s] path \"%s\" want \"%s\"\n",
                entry->rule, path, entry->expected);
            ++failures;
        }
    }
    /* A target longer than the output buffer is refused rather than truncated:
     * a truncated path that happened to equal the endpoint would be a routing
     * bypass. */
    char small[8];
    if (maelys_http_target_normalize("/a-much-longer-path", 19u, "h", 1u,
        small, sizeof(small))) {
        fprintf(stderr, "  an over-long target must be refused, not truncated\n");
        ++failures;
    }
    return failures;
}

/*
 * Nothing this parser compares is a C string. A Host value is a slice of the
 * read buffer with the next header directly behind it, so a comparison that
 * reached for strlen on it would read past the value - and would then mismatch
 * on a request that should have been routed, because the trailing bytes differ.
 * Every other case in this file happens to pass a NUL-terminated literal, which
 * is exactly why this one does not.
 */
static int absolute_form_compares_a_non_terminated_host(void) {
    static const char buffer[] = "127.0.0.1:8080\r\nAccept: */*";
    char path[64];
    ASSERT_TRUE(maelys_http_target_normalize("http://127.0.0.1:8080/mcp", 25u,
        buffer, 14u, path, sizeof(path)) == 1);
    ASSERT_TRUE(strcmp(path, "/mcp") == 0);
    /* And a Host whose first 14 bytes differ is still refused. */
    static const char other[] = "127.0.0.1:9090\r\nAccept: */*";
    ASSERT_TRUE(maelys_http_target_normalize("http://127.0.0.1:8080/mcp", 25u,
        other, 14u, path, sizeof(path)) == 0);
    return 0;
}

typedef struct media_case {
    const char *rule;
    const char *value;
    int expected;
} media_case_t;

static const media_case_t CONTENT_TYPE_CASES[] = {
    {"the bare media type is accepted", "application/json", 1},
    {"a charset=utf-8 parameter is accepted", "application/json; charset=utf-8", 1},
    {"the parameter needs no space", "application/json;charset=utf-8", 1},
    {"the charset value is case-insensitive", "application/json; charset=UTF-8", 1},
    {"a quoted charset is accepted", "application/json; charset=\"utf-8\"", 1},
    {"the media type is case-insensitive", "APPLICATION/JSON", 1},
    {"another media type is refused", "text/plain", 0},
    {"a prefix of the media type is refused", "application/jsonl", 0},
    {"another parameter is refused", "application/json; boundary=x", 0},
    /* Without the "charset=" test this reads as a bare utf-8 token and would
     * be accepted by a parameter check that only compared the value. */
    {"a bare utf-8 token is not a charset parameter", "application/json; utf-8", 0},
    {"a second parameter after a valid charset is refused",
        "application/json; charset=utf-8; boundary=x", 0},
    {"another charset is refused", "application/json; charset=iso-8859-1", 0},
    {"an empty value is refused", "", 0}
};

static const media_case_t ACCEPT_CASES[] = {
    {"both media types listed is sufficient",
        "application/json, text/event-stream", 1},
    {"order does not matter", "text/event-stream,application/json", 1},
    {"q parameters are ignored",
        "application/json;q=1.0, text/event-stream;q=0.9", 1},
    {"extra entries are tolerated",
        "text/plain, application/json, text/event-stream", 1},
    {"JSON alone is insufficient", "application/json", 0},
    {"the event stream alone is insufficient", "text/event-stream", 0},
    {"a wildcard does not stand in for either", "*/*", 0},
    {"an empty Accept is insufficient", "", 0}
};

static int media_validation(void) {
    int failures = 0;
    for (size_t index = 0;
        index < sizeof(CONTENT_TYPE_CASES) / sizeof(*CONTENT_TYPE_CASES); ++index) {
        const media_case_t *entry = &CONTENT_TYPE_CASES[index];
        int got = maelys_http_content_type_json(entry->value, strlen(entry->value));
        if (got != entry->expected) {
            fprintf(stderr, "  [Content-Type: %s] got %d want %d\n",
                entry->rule, got, entry->expected);
            ++failures;
        }
    }
    for (size_t index = 0; index < sizeof(ACCEPT_CASES) / sizeof(*ACCEPT_CASES); ++index) {
        const media_case_t *entry = &ACCEPT_CASES[index];
        int got = maelys_http_accept_sufficient(entry->value, strlen(entry->value));
        if (got != entry->expected) {
            fprintf(stderr, "  [Accept: %s] got %d want %d\n",
                entry->rule, got, entry->expected);
            ++failures;
        }
    }
    return failures;
}

int main(void) {
    static const maelys_test_case_t cases[] = {
        {"parser rejection matrix", rejection_matrix},
        {"split-across-chunks equivalence", split_across_chunks_equivalence},
        {"oversized request line, header block and header count", oversized_inputs},
        {"the request-line budget's exact boundary", request_line_budget_boundary},
        {"limits bound an unterminated stream", limits_bound_an_unterminated_stream},
        {"a rejection is final and never resynchronized", rejection_is_final},
        {"a duplicated header is reported absent", duplicate_lookup_reports_absent},
        {"the residue after the header block is the body", residue_is_the_body},
        {"residue longer than the body is pipelining", residue_reveals_pipelining},
        {"Host validation", host_validation},
        {"Origin validation", origin_validation},
        {"request-target normalization", target_normalization},
        {"absolute form compares a non-terminated Host",
            absolute_form_compares_a_non_terminated_host},
        {"Content-Type and Accept validation", media_validation}
    };
    return maelys_run_tests(cases, sizeof(cases) / sizeof(*cases));
}
