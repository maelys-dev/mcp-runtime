/*
 * http-smuggling: a generated header block drawn from a grammar that BIASES
 * TOWARD framing ambiguity.
 *
 * Separate from http-request on purpose. A uniformly random corpus almost never
 * produces two framing headers that disagree, so the ambiguity class has to be
 * generated deliberately rather than hoped for: this grammar emits
 * Content-Length and Transfer-Encoding in combinations of casing, repetition,
 * list form, whitespace and obs-fold, and lets the fuzzer pick among them.
 *
 * THE INVARIANT, and it is the strong one: the parser must return a single
 * unambiguous body length or refuse - never a length that depends on which
 * header it happened to read last. That is asserted here by re-reading the
 * generated block with an independent, deliberately naive second reader and
 * requiring the two to agree, which is the closest a single process can get to
 * "a front end and a back end cannot disagree about where this request ends".
 */
#include "host/http_parser.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Every spelling of a framing header worth confusing a parser with. The list is
 * the point of the target, so it is written out rather than generated: each
 * entry is a real desync primitive from the wild.
 */
static const char *const FRAMING[] = {
    "Content-Length: 5\r\n",
    "Content-Length: 5\r\n",
    "content-length: 5\r\n",
    "Content-Length:5\r\n",
    "Content-Length:  5  \r\n",
    "Content-Length: 05\r\n",
    "Content-Length: +5\r\n",
    "Content-Length: 5 \r\n",
    "Content-Length: 0x5\r\n",
    "Content-Length: 5, 5\r\n",
    "Content-Length: \r\n",
    "Content-Length: 99999999999999999999999999\r\n",
    "Transfer-Encoding: chunked\r\n",
    "transfer-encoding: chunked\r\n",
    "Transfer-Encoding: identity\r\n",
    "Transfer-Encoding: chunked, chunked\r\n",
    "Transfer-Encoding: chunked, identity\r\n",
    "Transfer-Encoding:\tchunked\r\n",
    "Transfer-Encoding: \r\n",
    "Transfer-Encoding : chunked\r\n",
    "Transfer_Encoding: chunked\r\n",
    "Content-Length: 5\r\n Transfer-Encoding: chunked\r\n",
    "Transfer-Encoding: chunked\r\n Content-Length: 5\r\n",
    "X-Filler: a\r\n",
    "Host: 127.0.0.1\r\n",
    "Content-Type: application/json\r\n"
};

#define FRAMING_COUNT (sizeof(FRAMING) / sizeof(*FRAMING))
#define BLOCK_CAPACITY 4096u

static int matches_at(const char *block, size_t length, size_t offset, const char *name) {
    size_t name_length = strlen(name);
    if (offset + name_length > length) return 0;
    for (size_t index = 0; index < name_length; ++index) {
        char left = block[offset + index];
        char right = name[index];
        if (left >= 'A' && left <= 'Z') left = (char)(left + 32);
        if (right >= 'A' && right <= 'Z') right = (char)(right + 32);
        if (left != right) return 0;
    }
    return 1;
}

/*
 * The second reader. Deliberately naive - it counts field lines by their name
 * prefix without any of the parser's structure - because a second reader that
 * shared the parser's code would agree with it by construction and prove
 * nothing. It only ever runs on a block the parser ACCEPTED, where the parser
 * has already guaranteed CRLF line endings and no obs-fold, so its naivety
 * cannot make it wrong about a block that got that far.
 */
static void count_framing(const char *block, size_t length,
    size_t *out_content_lengths, size_t *out_transfer_encodings) {
    size_t content_lengths = 0;
    size_t transfer_encodings = 0;
    size_t offset = 0;
    /* Skip the request line. */
    while (offset + 1u < length &&
        !(block[offset] == '\r' && block[offset + 1u] == '\n')) {
        ++offset;
    }
    offset += 2u;
    while (offset < length) {
        if (matches_at(block, length, offset, "content-length:")) ++content_lengths;
        if (matches_at(block, length, offset, "transfer-encoding:")) ++transfer_encodings;
        while (offset + 1u < length &&
            !(block[offset] == '\r' && block[offset + 1u] == '\n')) {
            ++offset;
        }
        offset += 2u;
    }
    *out_content_lengths = content_lengths;
    *out_transfer_encodings = transfer_encodings;
}

static size_t strict_length(const char *block, size_t length) {
    size_t offset = 0;
    while (offset + 1u < length &&
        !(block[offset] == '\r' && block[offset + 1u] == '\n')) {
        ++offset;
    }
    offset += 2u;
    while (offset < length) {
        if (matches_at(block, length, offset, "content-length:")) {
            size_t cursor = offset + strlen("content-length:");
            while (cursor < length && (block[cursor] == ' ' || block[cursor] == '\t')) {
                ++cursor;
            }
            size_t value = 0;
            while (cursor < length && block[cursor] >= '0' && block[cursor] <= '9') {
                value = value * 10u + (size_t)(block[cursor] - '0');
                ++cursor;
            }
            return value;
        }
        while (offset + 1u < length &&
            !(block[offset] == '\r' && block[offset + 1u] == '\n')) {
            ++offset;
        }
        offset += 2u;
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!size) return 0;
    char block[BLOCK_CAPACITY];
    size_t used = 0;
    static const char request_line[] = "POST /mcp HTTP/1.1\r\n";
    memcpy(block, request_line, sizeof(request_line) - 1u);
    used = sizeof(request_line) - 1u;

    /* Each fuzzer byte selects one field line, so the corpus's whole entropy
     * goes into WHICH ambiguities are combined rather than into rediscovering
     * that a header ends with CRLF. */
    for (size_t index = 0; index < size; ++index) {
        const char *field = FRAMING[data[index] % FRAMING_COUNT];
        size_t field_length = strlen(field);
        if (used + field_length + 2u >= BLOCK_CAPACITY) break;
        memcpy(block + used, field, field_length);
        used += field_length;
    }
    block[used++] = '\r';
    block[used++] = '\n';

    maelys_http_parser_t parser;
    maelys_http_parser_init(&parser);
    maelys_http_parse_state_t state = maelys_http_parser_feed(&parser, block, used);
    if (state == MAELYS_HTTP_PARSE_COMPLETE) {
        size_t content_lengths = 0;
        size_t transfer_encodings = 0;
        count_framing(block, used, &content_lengths, &transfer_encodings);
        /* Any Transfer-Encoding at all must have been refused. */
        assert(transfer_encodings == 0);
        /* A repeated Content-Length must have been refused, agreeing or not. */
        assert(content_lengths <= 1u);
        /* And the one length the parser reports must be the one the second
         * reader finds - not a different occurrence, not a truncation. */
        if (content_lengths == 1u) {
            assert(parser.has_content_length);
            assert(parser.content_length == strict_length(block, used));
        } else {
            assert(!parser.has_content_length);
        }
    } else {
        assert(state == MAELYS_HTTP_PARSE_REJECTED);
        assert(parser.reject != MAELYS_HTTP_REJECT_NONE);
    }
    maelys_http_parser_clear(&parser);
    return 0;
}
