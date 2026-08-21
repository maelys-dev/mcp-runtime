#include "host/http_parser.h"

#include <stdlib.h>
#include <string.h>

#define INITIAL_BUFFER_BYTES 4096u

/*
 * The protocol headers whose repetition is a 400 rather than a merge. The rule
 * is the design's ("A repeated occurrence of any header in the table is a 400,
 * not a merge") and its precedent is the sibling framing parser, which already
 * refuses a second Content-Length outright (src/jsonrpc/core.c:95); a transport
 * that merged where the sibling parser refuses would be the odd one out.
 *
 * Two entries need their reasoning recorded. Mcp-Param-* is in the design's
 * header table but is explicitly "not recognized by this server" and
 * forwarded-and-ignored per RFC 9110, so refusing a repeat of a field this
 * server has no opinion about would make its behaviour depend on that field;
 * the family is therefore absent here. Connection is not in that table, and is
 * here anyway: it decides whether the connection is reused, so two of them are
 * the same "which copy did you believe" ambiguity the rule exists to remove,
 * and treating a duplicate as absent would silently pick the more permissive
 * answer.
 */
static const char *const REFUSE_DUPLICATE[] = {
    "host",
    "content-type",
    "content-length",
    "transfer-encoding",
    "accept",
    "expect",
    "origin",
    "authorization",
    "connection",
    "mcp-protocol-version",
    "mcp-method",
    "mcp-name"
};

static int is_tchar(unsigned char byte) {
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9')) {
        return 1;
    }
    switch (byte) {
        case '!': case '#': case '$': case '%': case '&': case '\'':
        case '*': case '+': case '-': case '.': case '^': case '_':
        case '`': case '|': case '~':
            return 1;
        default:
            return 0;
    }
}

static unsigned char lower(unsigned char byte) {
    return (byte >= 'A' && byte <= 'Z') ? (unsigned char)(byte + 32) : byte;
}

/*
 * Both sides length-explicit. Nothing this parser compares is a C string:
 * header values, authorities and targets are all slices of the read buffer, and
 * a comparison that reached for strlen on one of them would read past the value
 * into the next header.
 */
static int equals_ci_bytes(
    const char *left, size_t left_length,
    const char *right, size_t right_length) {
    if (left_length != right_length) return 0;
    for (size_t index = 0; index < left_length; ++index) {
        if (lower((unsigned char)left[index]) != lower((unsigned char)right[index])) {
            return 0;
        }
    }
    return 1;
}

/* The literal-on-the-right convenience. Only ever called with a string constant
 * as `literal`, which is what makes the strlen here safe. */
static int equals_ci(const char *bytes, size_t length, const char *literal) {
    return equals_ci_bytes(bytes, length, literal, strlen(literal));
}

static int hex_value(unsigned char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

void maelys_http_parser_init(maelys_http_parser_t *parser) {
    if (!parser) return;
    memset(parser, 0, sizeof(*parser));
    parser->max_request_line_bytes = MAELYS_HTTP_MAX_REQUEST_LINE_BYTES;
    parser->max_header_bytes = MAELYS_HTTP_MAX_HEADER_BYTES;
    parser->max_header_count = MAELYS_HTTP_MAX_HEADER_COUNT;
    parser->state = MAELYS_HTTP_PARSE_INCOMPLETE;
}

void maelys_http_parser_clear(maelys_http_parser_t *parser) {
    if (!parser) return;
    free(parser->buffer);
    parser->buffer = NULL;
    parser->length = 0;
    parser->capacity = 0;
}

/* The append half of src/jsonrpc/core.c's ring, overflow checks included. */
static int append_bytes(maelys_http_parser_t *parser, const char *bytes, size_t length) {
    if (!length) return 0;
    if (length > (size_t)-1 - parser->length) return -1;
    size_t required = parser->length + length;
    if (required > parser->capacity) {
        size_t capacity = parser->capacity ? parser->capacity : INITIAL_BUFFER_BYTES;
        while (capacity < required) {
            if (capacity > (size_t)-1 / 2u) return -1;
            capacity *= 2u;
        }
        char *grown = realloc(parser->buffer, capacity);
        if (!grown) return -1;
        parser->buffer = grown;
        parser->capacity = capacity;
    }
    memcpy(parser->buffer + parser->length, bytes, length);
    parser->length += length;
    return 0;
}

static maelys_http_parse_state_t fail(
    maelys_http_parser_t *parser,
    maelys_http_reject_t reject) {
    parser->state = MAELYS_HTTP_PARSE_REJECTED;
    parser->reject = reject;
    return parser->state;
}

/*
 * Content-Length, exactly 1*DIGIT and nothing else: no leading '+', no sign, no
 * surrounding or embedded space, no hex, no "0x", no empty value. Overflow is
 * refused rather than truncated - src/jsonrpc/core.c:76 is the arithmetic.
 */
static int parse_content_length(const char *value, size_t length, size_t *out) {
    if (!length) return -1;
    size_t result = 0;
    for (size_t index = 0; index < length; ++index) {
        unsigned char byte = (unsigned char)value[index];
        if (byte < '0' || byte > '9') return -1;
        size_t digit = (size_t)(byte - '0');
        if (result > ((size_t)-1 - digit) / 10u) return -1;
        result = result * 10u + digit;
    }
    *out = result;
    return 0;
}

static int header_name_is(
    const maelys_http_parser_t *parser,
    const maelys_http_header_t *header,
    const char *name) {
    return equals_ci(parser->buffer + header->name_offset, header->name_length, name);
}

static size_t count_header(const maelys_http_parser_t *parser, const char *name) {
    size_t count = 0;
    for (size_t index = 0; index < parser->header_count; ++index) {
        if (header_name_is(parser, &parser->headers[index], name)) ++count;
    }
    return count;
}

/*
 * The request line, in exactly three space-separated parts. A leading space
 * gives an empty method and is refused there rather than skipped, which is the
 * same refusal-not-resynchronization stance the rest of the parser takes.
 */
static maelys_http_parse_state_t parse_request_line(
    maelys_http_parser_t *parser,
    size_t line_length) {
    const char *line = parser->buffer;
    size_t first = 0;
    while (first < line_length && line[first] != ' ') ++first;
    if (first == 0 || first >= line_length) {
        /* No space at all, or an empty method. A two-token line is HTTP/0.9,
         * which is a version this server does not speak rather than a
         * malformed line. */
        return fail(parser, MAELYS_HTTP_REJECT_BAD_REQUEST);
    }
    size_t second = first + 1u;
    while (second < line_length && line[second] != ' ') ++second;
    size_t target_length = second - (first + 1u);
    if (!target_length) return fail(parser, MAELYS_HTTP_REJECT_BAD_REQUEST);
    for (size_t index = 0; index < first; ++index) {
        if (!is_tchar((unsigned char)line[index])) {
            return fail(parser, MAELYS_HTTP_REJECT_BAD_REQUEST);
        }
    }
    /* The target must be VCHAR throughout: a space would have ended it, and a
     * control byte or a NUL never reaches the normalizer. */
    for (size_t index = first + 1u; index < second; ++index) {
        unsigned char byte = (unsigned char)line[index];
        if (byte < 0x21u || byte > 0x7eu) {
            return fail(parser, MAELYS_HTTP_REJECT_BAD_REQUEST);
        }
    }
    if (second >= line_length) {
        /* Two tokens: an HTTP/0.9 simple request. */
        return fail(parser, MAELYS_HTTP_REJECT_VERSION);
    }
    const char *version = line + second + 1u;
    size_t version_length = line_length - (second + 1u);
    if (version_length == 8u && memcmp(version, "HTTP/1.1", 8u) == 0) {
        parser->method_offset = 0;
        parser->method_length = first;
        parser->target_offset = first + 1u;
        parser->target_length = target_length;
        return MAELYS_HTTP_PARSE_INCOMPLETE;
    }
    /*
     * HTTP/1.0 is refused rather than downgraded to, because its connection
     * semantics are a second state machine. Anything shaped like a version gets
     * 505 so the client learns which half of the request was wrong; anything
     * else is a malformed line.
     */
    if (version_length >= 6u && memcmp(version, "HTTP/", 5u) == 0) {
        return fail(parser, MAELYS_HTTP_REJECT_VERSION);
    }
    return fail(parser, MAELYS_HTTP_REJECT_BAD_REQUEST);
}

static maelys_http_parse_state_t parse_header_line(
    maelys_http_parser_t *parser,
    size_t line_offset,
    size_t line_length) {
    const char *line = parser->buffer + line_offset;
    /*
     * obs-fold. A continuation line is a second way to spell a header value and
     * therefore a second thing two parsers can disagree about.
     *
     * The tchar test below would refuse the same line anyway, since neither SP
     * nor HTAB is a tchar and a folded line's "name" therefore cannot be one.
     * This stays as its own check because the design names obs-fold as its own
     * rule, and a rule enforced only as a side effect of a different rule is a
     * rule that disappears the day the other one is refactored.
     */
    if (line[0] == ' ' || line[0] == '\t') {
        return fail(parser, MAELYS_HTTP_REJECT_BAD_REQUEST);
    }
    size_t colon = 0;
    while (colon < line_length && line[colon] != ':') ++colon;
    if (colon == 0 || colon >= line_length) {
        return fail(parser, MAELYS_HTTP_REJECT_BAD_REQUEST);
    }
    /* Whitespace between the field name and its colon is refused by the tchar
     * test below, because neither SP nor HTAB is a tchar. */
    for (size_t index = 0; index < colon; ++index) {
        if (!is_tchar((unsigned char)line[index])) {
            return fail(parser, MAELYS_HTTP_REJECT_BAD_REQUEST);
        }
    }
    size_t value_start = colon + 1u;
    size_t value_end = line_length;
    while (value_start < value_end &&
        (line[value_start] == ' ' || line[value_start] == '\t')) {
        ++value_start;
    }
    while (value_end > value_start &&
        (line[value_end - 1u] == ' ' || line[value_end - 1u] == '\t')) {
        --value_end;
    }
    /*
     * NUL, CR or LF anywhere in a value, refused before the value is
     * interpreted by anything. A bare LF cannot reach here - the scan refuses
     * it - but a bare CR inside a line can, and the same class of defect is
     * what the runtime already rejects wherever protocol strings enter
     * length-unaware C APIs (docs/security-model.md:60).
     */
    for (size_t index = value_start; index < value_end; ++index) {
        unsigned char byte = (unsigned char)line[index];
        if (byte == 0u || byte == '\r' || byte == '\n') {
            return fail(parser, MAELYS_HTTP_REJECT_BAD_REQUEST);
        }
    }
    if (parser->header_count >= parser->max_header_count ||
        parser->header_count >= MAELYS_HTTP_MAX_HEADER_COUNT) {
        return fail(parser, MAELYS_HTTP_REJECT_HEADERS_TOO_LARGE);
    }
    maelys_http_header_t *header = &parser->headers[parser->header_count++];
    header->name_offset = line_offset;
    header->name_length = colon;
    header->value_offset = line_offset + value_start;
    header->value_length = value_end - value_start;
    return MAELYS_HTTP_PARSE_INCOMPLETE;
}

/*
 * Framing, decided once. The order matters: Transfer-Encoding together with
 * Content-Length is reported as smuggling rather than as a malformed request
 * even though the blanket Transfer-Encoding refusal would have caught it
 * anyway, because it is the single most exploited desync primitive and a log
 * line that calls it "malformed" buries it.
 */
static maelys_http_parse_state_t resolve_framing(maelys_http_parser_t *parser) {
    size_t transfer_encodings = count_header(parser, "transfer-encoding");
    size_t content_lengths = count_header(parser, "content-length");
    if (transfer_encodings && content_lengths) {
        return fail(parser, MAELYS_HTTP_REJECT_SMUGGLING);
    }
    /* Not merely "chunked": any value, any casing, any list, including
     * identity and the classic chunked, chunked obfuscations. There is one
     * inbound framing on this endpoint and refusing the alternative outright is
     * what removes the smuggling class rather than defending against it. */
    if (transfer_encodings) return fail(parser, MAELYS_HTTP_REJECT_BAD_REQUEST);
    /*
     * A repeated Content-Length is refused even when the values agree -
     * agreeing duplicates are what a front end and a back end disagree about
     * later - and refuse_duplicates above is the single owner of that rule.
     * A second check here would be one no test could distinguish from the
     * first, which is how a rule ends up with two homes and no owner.
     */
    if (content_lengths == 1u) {
        for (size_t index = 0; index < parser->header_count; ++index) {
            const maelys_http_header_t *header = &parser->headers[index];
            if (!header_name_is(parser, header, "content-length")) continue;
            size_t value = 0;
            if (parse_content_length(parser->buffer + header->value_offset,
                header->value_length, &value) != 0) {
                return fail(parser, MAELYS_HTTP_REJECT_BAD_REQUEST);
            }
            parser->has_content_length = 1;
            parser->content_length = value;
        }
    }
    return MAELYS_HTTP_PARSE_INCOMPLETE;
}

static maelys_http_parse_state_t refuse_duplicates(maelys_http_parser_t *parser) {
    for (size_t index = 0; index < sizeof(REFUSE_DUPLICATE) / sizeof(*REFUSE_DUPLICATE);
        ++index) {
        if (count_header(parser, REFUSE_DUPLICATE[index]) > 1u) {
            return fail(parser, MAELYS_HTTP_REJECT_BAD_REQUEST);
        }
    }
    return MAELYS_HTTP_PARSE_INCOMPLETE;
}

/*
 * One pass over the completed block. Splitting the scan (which is incremental,
 * and whose only jobs are refusing a bare LF, enforcing the limits and finding
 * the terminator) from the parse (which runs once, over a block that is
 * already whole) is what makes the split-across-chunks equivalence property
 * true by construction rather than by testing: the parse never sees a chunk
 * boundary.
 */
static maelys_http_parse_state_t parse_block(
    maelys_http_parser_t *parser,
    size_t block_length) {
    size_t offset = 0;
    size_t line_index = 0;
    while (offset < block_length) {
        size_t end = offset;
        while (end + 1u < block_length &&
            !(parser->buffer[end] == '\r' && parser->buffer[end + 1u] == '\n')) {
            ++end;
        }
        size_t line_length = end - offset;
        if (line_index == 0) {
            if (parse_request_line(parser, line_length) == MAELYS_HTTP_PARSE_REJECTED) {
                return parser->state;
            }
        } else if (line_length > 0) {
            if (parse_header_line(parser, offset, line_length) ==
                MAELYS_HTTP_PARSE_REJECTED) {
                return parser->state;
            }
        }
        ++line_index;
        offset = end + 2u;
    }
    if (refuse_duplicates(parser) == MAELYS_HTTP_PARSE_REJECTED) return parser->state;
    if (resolve_framing(parser) == MAELYS_HTTP_PARSE_REJECTED) return parser->state;
    parser->state = MAELYS_HTTP_PARSE_COMPLETE;
    return parser->state;
}

maelys_http_parse_state_t maelys_http_parser_feed(
    maelys_http_parser_t *parser,
    const void *bytes,
    size_t length) {
    if (!parser) return MAELYS_HTTP_PARSE_REJECTED;
    if (parser->state != MAELYS_HTTP_PARSE_INCOMPLETE) return parser->state;
    if (!bytes && length) return fail(parser, MAELYS_HTTP_REJECT_BAD_REQUEST);
    if (append_bytes(parser, bytes, length) != 0) {
        return fail(parser, MAELYS_HTTP_REJECT_HEADERS_TOO_LARGE);
    }
    /*
     * Any whitespace before the request line, a leading CRLF included. The
     * RFC's tolerance for a leading empty line is a resynchronization aid and
     * this parser does not resynchronize.
     *
     * Each of the four bytes would also be refused downstream - a leading SP or
     * HTAB makes the method empty or non-tchar, a leading CRLF makes the
     * request line empty - so this check is belt to their braces. It is kept
     * because "the request line starts at byte zero" is the precondition every
     * offset in this parser is written against, and checking it once at the
     * door is cheaper than reasoning about it at each of them.
     */
    if (parser->length > 0) {
        char first = parser->buffer[0];
        if (first == ' ' || first == '\t' || first == '\r' || first == '\n') {
            return fail(parser, MAELYS_HTTP_REJECT_BAD_REQUEST);
        }
    }
    while (parser->scanned < parser->length) {
        size_t index = parser->scanned;
        char byte = parser->buffer[index];
        if (byte == '\n') {
            /* Bare LF as a line terminator anywhere. CRLF only. */
            if (index == 0 || parser->buffer[index - 1u] != '\r') {
                return fail(parser, MAELYS_HTTP_REJECT_BAD_REQUEST);
            }
            size_t line_length = (index - 1u) - parser->line_start;
            if (!parser->have_header_start) {
                if (line_length > parser->max_request_line_bytes) {
                    return fail(parser, MAELYS_HTTP_REJECT_HEADERS_TOO_LARGE);
                }
                parser->have_header_start = 1;
                parser->header_start = index + 1u;
            } else if (line_length == 0) {
                /* The empty line: the block is whole, and everything past this
                 * LF belongs to the caller. */
                parser->scanned = index + 1u;
                parser->header_block_length = index + 1u;
                return parse_block(parser, parser->line_start);
            }
            parser->line_start = index + 1u;
        }
        ++parser->scanned;
        /*
         * The limits, enforced while the block is still growing rather than
         * after it is whole - an endless header stream must be refused without
         * ever having been buffered.
         */
        if (!parser->have_header_start) {
            /*
             * The budget is the request LINE, so the CRLF that ends it is not
             * charged against it - hence the +2. Without that the check above
             * and this one disagree by two bytes about where the limit is, and
             * a request line of exactly the limit would be refused by the
             * incremental check before the exact one ever ran.
             *
             * The two are not redundant. This one bounds a request line that
             * never terminates, which is the only case the check above cannot
             * see; that one enforces the exact boundary, which this one cannot.
             */
            if (parser->scanned > parser->max_request_line_bytes + 2u) {
                return fail(parser, MAELYS_HTTP_REJECT_HEADERS_TOO_LARGE);
            }
        } else if (parser->scanned - parser->header_start > parser->max_header_bytes) {
            return fail(parser, MAELYS_HTTP_REJECT_HEADERS_TOO_LARGE);
        }
    }
    return parser->state;
}

const char *maelys_http_parser_residue(
    const maelys_http_parser_t *parser,
    size_t *out_length) {
    if (!parser || parser->state != MAELYS_HTTP_PARSE_COMPLETE) {
        if (out_length) *out_length = 0;
        return NULL;
    }
    if (out_length) *out_length = parser->length - parser->header_block_length;
    return parser->buffer + parser->header_block_length;
}

int maelys_http_parser_header(
    const maelys_http_parser_t *parser,
    const char *name,
    const char **out_value,
    size_t *out_length) {
    if (!parser || !name) return 0;
    const maelys_http_header_t *found = NULL;
    for (size_t index = 0; index < parser->header_count; ++index) {
        if (!header_name_is(parser, &parser->headers[index], name)) continue;
        /* Present more than once is reported absent, never merged and never
         * "the first one wins". */
        if (found) return 0;
        found = &parser->headers[index];
    }
    if (!found) return 0;
    if (out_value) *out_value = parser->buffer + found->value_offset;
    if (out_length) *out_length = found->value_length;
    return 1;
}

const char *maelys_http_parser_method(
    const maelys_http_parser_t *parser, size_t *out_length) {
    if (!parser || parser->state != MAELYS_HTTP_PARSE_COMPLETE) return NULL;
    if (out_length) *out_length = parser->method_length;
    return parser->buffer + parser->method_offset;
}

const char *maelys_http_parser_target(
    const maelys_http_parser_t *parser, size_t *out_length) {
    if (!parser || parser->state != MAELYS_HTTP_PARSE_COMPLETE) return NULL;
    if (out_length) *out_length = parser->target_length;
    return parser->buffer + parser->target_offset;
}

static int is_loopback_authority(const char *host, size_t length) {
    if (equals_ci(host, length, "localhost")) return 1;
    if (equals_ci(host, length, "::1")) return 1;
    if (equals_ci(host, length, "0:0:0:0:0:0:0:1")) return 1;
    /* 127.0.0.0/8, which is the whole loopback range and not only 127.0.0.1. */
    unsigned int octet[4] = {0, 0, 0, 0};
    size_t index = 0;
    for (size_t part = 0; part < 4u; ++part) {
        if (part && (index >= length || host[index] != '.')) return 0;
        if (part) ++index;
        size_t digits = 0;
        unsigned int value = 0;
        while (index < length && host[index] >= '0' && host[index] <= '9') {
            value = value * 10u + (unsigned int)(host[index] - '0');
            if (value > 255u) return 0;
            ++index;
            ++digits;
        }
        if (!digits || digits > 3u) return 0;
        octet[part] = value;
    }
    if (index != length) return 0;
    return octet[0] == 127u;
}

int maelys_http_host_valid(const char *value, size_t length, int expect_loopback) {
    if (!value || !length) return 0;
    size_t host_start = 0;
    size_t host_end = 0;
    size_t port_start = length;
    if (value[0] == '[') {
        size_t close = 1;
        while (close < length && value[close] != ']') ++close;
        if (close >= length || close == 1u) return 0;
        for (size_t index = 1; index < close; ++index) {
            unsigned char byte = (unsigned char)value[index];
            if (hex_value(byte) < 0 && byte != ':' && byte != '.' && byte != '%') return 0;
        }
        host_start = 1;
        host_end = close;
        port_start = close + 1u;
    } else {
        size_t colon = length;
        for (size_t index = 0; index < length; ++index) {
            if (value[index] == ':') {
                /* An unbracketed IPv6 literal has several, and is not a valid
                 * Host. One colon means a port follows. */
                if (colon != length) return 0;
                colon = index;
            }
        }
        host_start = 0;
        host_end = colon;
        port_start = colon;
        if (host_end == 0) return 0;
        for (size_t index = 0; index < host_end; ++index) {
            unsigned char byte = (unsigned char)value[index];
            int allowed = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
                byte == '_' || byte == '~';
            if (!allowed) return 0;
        }
    }
    if (port_start < length) {
        if (value[port_start] != ':') return 0;
        size_t digits = 0;
        unsigned long port = 0;
        for (size_t index = port_start + 1u; index < length; ++index) {
            if (value[index] < '0' || value[index] > '9') return 0;
            port = port * 10u + (unsigned long)(value[index] - '0');
            if (port > 65535u) return 0;
            ++digits;
        }
        if (!digits) return 0;
    }
    if (expect_loopback) {
        return is_loopback_authority(value + host_start, host_end - host_start);
    }
    return 1;
}

int maelys_http_origin_allowed(
    const char *value,
    size_t length,
    const char *const *allowed,
    size_t allowed_count,
    int bind_is_loopback) {
    /*
     * Absent. Accepted only on a loopback bind: a browser always sends Origin
     * on a cross-origin request, so "no Origin" on a public bind is either a
     * non-browser client that can equally well send one or an attempt to skip
     * the check.
     */
    if (!value) return bind_is_loopback;
    if (!length) return 0;
    for (size_t index = 0; index < length; ++index) {
        unsigned char byte = (unsigned char)value[index];
        if (byte == 0u || byte == '\r' || byte == '\n') return 0;
    }
    /* Byte-for-byte against the allowlist, which is empty by default. Exact
     * comparison rather than a case-folded or normalized one, because every
     * normalization is one more way for two values to be "the same" origin. */
    for (size_t index = 0; index < allowed_count; ++index) {
        if (!allowed[index]) continue;
        size_t candidate_length = strlen(allowed[index]);
        if (candidate_length == length && memcmp(allowed[index], value, length) == 0) {
            return 1;
        }
    }
    return 0;
}

int maelys_http_target_normalize(
    const char *target,
    size_t target_length,
    const char *host,
    size_t host_length,
    char *out,
    size_t out_capacity) {
    if (!target || !target_length || !out || !out_capacity) return 0;
    /*
     * Asterisk-form and authority-form. Neither addresses a resource on this
     * endpoint, and accepting either would mean a second target grammar.
     * The origin-form test further down refuses both anyway; this names the
     * asterisk case explicitly because it is the one the design calls out and
     * the one a reader looks for.
     */
    if (target[0] == '*') return 0;
    const char *path = target;
    size_t path_length = target_length;
    if ((target_length > 7u && equals_ci(target, 7u, "http://")) ||
        (target_length > 8u && equals_ci(target, 8u, "https://"))) {
        size_t scheme = target[4] == 's' ? 8u : 7u;
        size_t authority_end = scheme;
        while (authority_end < target_length && target[authority_end] != '/') ++authority_end;
        size_t authority_length = authority_end - scheme;
        /* An absolute-form target whose authority does not match Host is
         * refused: two names for one request is exactly the ambiguity the Host
         * rule exists to remove. */
        if (!host || !equals_ci_bytes(target + scheme, authority_length,
            host, host_length)) {
            return 0;
        }
        path = target + authority_end;
        path_length = target_length - authority_end;
        if (!path_length) {
            path = "/";
            path_length = 1u;
        }
    }
    if (path[0] != '/') return 0;
    size_t written = 0;
    for (size_t index = 0; index < path_length; ++index) {
        unsigned char byte = (unsigned char)path[index];
        if (byte == '%') {
            /* Exactly one decoding pass, and a malformed escape is a
             * rejection rather than a literal percent. */
            if (index + 2u >= path_length) return 0;
            int high = hex_value((unsigned char)path[index + 1u]);
            int low = hex_value((unsigned char)path[index + 2u]);
            if (high < 0 || low < 0) return 0;
            byte = (unsigned char)((high << 4) | low);
            index += 2u;
        }
        /* A NUL or a control byte in the decoded target, which is where
         * percent-encoding is usually smuggling one. */
        if (byte < 0x20u || byte == 0x7fu) return 0;
        if (written + 1u >= out_capacity) return 0;
        out[written++] = (char)byte;
    }
    out[written] = '\0';
    /* `..` segments, checked after decoding so that %2e%2e and %2f are caught
     * by the same test as the literal spelling. */
    size_t segment_start = 0;
    for (size_t index = 0; index <= written; ++index) {
        if (index == written || out[index] == '/') {
            size_t segment_length = index - segment_start;
            if (segment_length == 2u && out[segment_start] == '.' &&
                out[segment_start + 1u] == '.') {
                return 0;
            }
            segment_start = index + 1u;
        }
    }
    return 1;
}

int maelys_http_content_type_json(const char *value, size_t length) {
    if (!value || !length) return 0;
    size_t index = 0;
    while (index < length && value[index] != ';') ++index;
    size_t media_end = index;
    while (media_end > 0 && (value[media_end - 1u] == ' ' || value[media_end - 1u] == '\t')) {
        --media_end;
    }
    if (!equals_ci(value, media_end, "application/json")) return 0;
    if (index >= length) return 1;
    ++index;
    while (index < length && (value[index] == ' ' || value[index] == '\t')) ++index;
    /* The one parameter this endpoint tolerates. Anything else is a media type
     * it does not serve, not a media type it serves with extras. */
    if (index + 8u > length || !equals_ci(value + index, 8u, "charset=")) return 0;
    index += 8u;
    size_t rest = length - index;
    if (rest >= 2u && value[index] == '"' && value[length - 1u] == '"') {
        return equals_ci(value + index + 1u, rest - 2u, "utf-8");
    }
    return equals_ci(value + index, rest, "utf-8");
}

int maelys_http_accept_sufficient(const char *value, size_t length) {
    if (!value || !length) return 0;
    int json = 0;
    int stream = 0;
    size_t index = 0;
    while (index < length) {
        size_t start = index;
        while (index < length && value[index] != ',') ++index;
        size_t end = index;
        if (index < length) ++index;
        size_t media_end = start;
        while (media_end < end && value[media_end] != ';') ++media_end;
        size_t trimmed_start = start;
        while (trimmed_start < media_end &&
            (value[trimmed_start] == ' ' || value[trimmed_start] == '\t')) {
            ++trimmed_start;
        }
        size_t trimmed_end = media_end;
        while (trimmed_end > trimmed_start &&
            (value[trimmed_end - 1u] == ' ' || value[trimmed_end - 1u] == '\t')) {
            --trimmed_end;
        }
        size_t media_length = trimmed_end - trimmed_start;
        if (equals_ci(value + trimmed_start, media_length, "application/json")) json = 1;
        if (equals_ci(value + trimmed_start, media_length, "text/event-stream")) stream = 1;
    }
    /* Both, named explicitly. A wildcard is not accepted in their place: the
     * response mode is chosen per request by what the channel produces, so a
     * client that has not said it can read an event stream cannot be sent one,
     * and discovering that after the status line is written is too late. */
    return json && stream;
}
