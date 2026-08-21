/*
 * http-request: arbitrary bytes, in arbitrary chunk sizes, into the request-line
 * and header parser.
 *
 * The invariant this target exists for is the one an incremental parser is
 * always at risk of losing: feeding the same input in one chunk and in random
 * chunks must produce the SAME decision. fuzz/fuzz_content_length.c already
 * fuzzes exactly that shape for the sibling framer, and the chunking scheme
 * here is deliberately the same one.
 *
 * It also holds the parser to its own accounting: on acceptance the header
 * block plus the residue must be exactly the bytes that were fed, so there is
 * no offset at which the parser and its caller could disagree about where the
 * body starts.
 */
#include "host/http_parser.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

typedef struct verdict {
    maelys_http_parse_state_t state;
    maelys_http_reject_t reject;
    int has_content_length;
    size_t content_length;
    size_t header_count;
    size_t header_block_length;
} verdict_t;

static verdict_t drive(const uint8_t *data, size_t size, int chunked) {
    maelys_http_parser_t parser;
    maelys_http_parser_init(&parser);
    maelys_http_parse_state_t state = MAELYS_HTTP_PARSE_INCOMPLETE;
    size_t offset = 0;
    while (offset < size && state == MAELYS_HTTP_PARSE_INCOMPLETE) {
        size_t chunk = size - offset;
        if (chunked) {
            chunk = 1u + (size_t)(data[offset] % 47u);
            if (chunk > size - offset) chunk = size - offset;
        }
        state = maelys_http_parser_feed(&parser, data + offset, chunk);
        offset += chunk;
    }
    verdict_t verdict = {
        .state = state,
        .reject = parser.reject,
        .has_content_length = parser.has_content_length,
        .content_length = parser.content_length,
        .header_count = parser.header_count,
        .header_block_length = parser.header_block_length
    };
    if (state == MAELYS_HTTP_PARSE_COMPLETE) {
        size_t residue_length = 0;
        const char *residue = maelys_http_parser_residue(&parser, &residue_length);
        assert(residue != NULL);
        /* Everything that was fed is either header block or residue, with
         * nothing counted twice and nothing lost. */
        assert(parser.header_block_length + residue_length == parser.length);
        assert(parser.header_block_length <= parser.length);
    } else {
        size_t residue_length = 0;
        /* Nothing past an unfinished or refused block is claimed to be a body. */
        assert(maelys_http_parser_residue(&parser, &residue_length) == NULL);
        assert(residue_length == 0);
    }
    maelys_http_parser_clear(&parser);
    return verdict;
}

static int same(verdict_t left, verdict_t right) {
    return left.state == right.state &&
        left.reject == right.reject &&
        left.has_content_length == right.has_content_length &&
        left.content_length == right.content_length &&
        left.header_count == right.header_count &&
        left.header_block_length == right.header_block_length;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!size) return 0;
    verdict_t whole = drive(data, size, 0);
    verdict_t split = drive(data, size, 1);
    /* The whole point of the target. A parser that disagrees with itself across
     * a chunk boundary is a parser two proxies can be made to disagree about. */
    assert(same(whole, split));
    return 0;
}
