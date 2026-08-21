/*
 * The HTTP <-> MCP adapter, H3 shape: the phase in which it starts serving MCP.
 *
 * H1 built both sides of the seam in include/maelys/mcp/http.h and answered
 * every exchange from a table. H2 put the MCP validation in front of that
 * table. H3 deletes the table. A request that survives validation now creates a
 * channel bound to the caller's principal and to MAELYS_MCP_ERA_MODERN, hands
 * the frame to maelys_mcp_channel_accept, and drains that channel's outbox
 * until the request's own response comes out of it.
 *
 * WHAT THIS FILE MAY PRODUCE. The design's status table carries a "Produced by"
 * column and calls it "the layering made checkable". Three of its rows name the
 * adapter as the DECIDER:
 *
 *   body not valid JSON / not a JSON object   400  -32700
 *   body is a JSON-RPC response               400  -32600, no id
 *   header missing, malformed, or != body     400  -32020  HeaderMismatch
 *
 * Everything below them - -32022, -32601, -32021, -32002, -32003 - is "a
 * dispatch result the transport reports and never rewrites". H3 is where those
 * rows become reachable, and status_for_response below is the whole of the
 * reporting: it reads the code out of a response the runtime produced and picks
 * the row's status. It rewrites nothing, and the one case it must never rewrite
 * is the policy denial, which stays 200 with -32003 because an authenticated,
 * well-formed request that policy refuses is a normal MCP exchange whose answer
 * is a refusal - identical over stdio, and not the transport's business.
 *
 * WHAT THIS FILE STILL MAY NOT DO. No descriptor here is a socket. The two it
 * polls are an abstract cancellation source and an abstract shutdown source,
 * both supplied by the embedder, and it reads from neither; the third is the
 * channel's own outbox wakeup. Telling a FIN from a pipelined byte needs the
 * socket and the pipelining rule, and both live in the server layer.
 */
#include "src/internal/internal.h"

#include "maelys/mcp/http.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * The Base64 sentinel. Prefix and suffix are lowercase and case-sensitive, and
 * a value must carry both to be decoded at all.
 *
 * The 11-byte floor is not decoration: at 10 bytes "=?base64?=" satisfies both
 * memcmp tests while the prefix and the suffix overlap, so a payload length
 * computed as length - 11 would underflow. Below the floor the value is taken
 * literally, which is also the right answer - it is not a well-formed sentinel.
 */
#define SENTINEL_PREFIX "=?base64?"
#define SENTINEL_PREFIX_BYTES 9u
#define SENTINEL_SUFFIX_BYTES 2u
#define SENTINEL_MIN_BYTES (SENTINEL_PREFIX_BYTES + SENTINEL_SUFFIX_BYTES)

struct maelys_mcp_http_adapter {
    maelys_mcp_runtime_t *runtime;
    /* The template every exchange's channel is created from. `context` and
     * `context_release` are the two fields it deliberately does not carry: they
     * are per-principal and therefore per-request. */
    maelys_mcp_channel_config_t channel_template;
    unsigned int keepalive_interval_ms;
    unsigned int close_timeout_ms;
};

/*
 * The handle a canceller names. `cancelled` and `shutdown` are written by
 * maelys_mcp_http_exchange_cancel / _shutdown from any thread and read by
 * _handle, all under `mutex`; `live` is what makes a late call a no-op by
 * construction rather than by the caller's discipline.
 *
 * The wakeup pipe exists only for the embedder that supplied NEITHER
 * cancel_fd nor shutdown_fd. An embedder with a descriptor of its own makes
 * that descriptor readable and pays for no pipe here; an embedder with none
 * would otherwise set a flag nobody would look at until the keep-alive interval
 * expired, which is a cancellation latency this design refuses to have.
 */
struct maelys_mcp_http_exchange {
    pthread_mutex_t mutex;
    int live;
    int cancelled;
    int shutdown;
    int wake_read;
    int wake_write;
};

/* --------------------------------------------------------------- Base64 */

static int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
    if (c >= 'a' && c <= 'z') return (int)(c - 'a') + 26;
    if (c >= '0' && c <= '9') return (int)(c - '0') + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    /* No URL-safe alphabet: '-' and '_' are not accepted, and neither is any
     * whitespace, so there is no line wrapping either.
     *
     * -1 rather than 0, and the distinction is not stylistic: 0 is 'A'. A
     * rejection that returns an in-range index does not reject, it substitutes,
     * and every unrecognised byte would decode as 'A'. */
    return -1;
}

/*
 * Standard Base64 with '=' padding, decoded strictly. Every rejection here is
 * a design rule: a non-alphabet character, a length that is not a multiple of
 * four, padding anywhere but the tail, more than two padding bytes, and
 * non-zero bits in the padding. The last one is the rule that stops two
 * different encodings from decoding to the same bytes, which is what would let
 * a client produce a header that survives comparison in more than one form.
 *
 * Returns 1 on success, with *out malloc'd and *out_length set.
 *
 * THE LENGTH CHECK IS MEMORY SAFETY, not tidiness, and the distinction took a
 * mutation under ASan to establish. `capacity` is (length / 4) * 3, which is
 * the exact size of a whole number of groups. Remove the check and a payload
 * whose length is 2 or 3 mod 4 writes past that buffer: at length 6, capacity
 * is 3, and the second group writes a fourth byte BEFORE the alphabet test on
 * the byte after it gets a chance to refuse. A payload of length 1 mod 4 does
 * not show this - its trailing group is refused before any write - so a test
 * that happens to pick that length proves nothing about the rule.
 *
 * The padding-position check is the one that is genuinely redundant here, and
 * it stays anyway. decode_mcp_name always hands over a slice whose next byte is
 * the sentinel's '?', and an '=' in the interior is refused by the alphabet
 * test regardless - but a decoder whose safety depends on its caller's framing
 * is a decoder that breaks the first time it is reused.
 *
 * The alphabet test both of them lean on must return a value that is not a
 * valid index. `-0` is `0`, which is 'A', so folding an unrecognised byte to
 * zero would silently decode it - see base64_value.
 */
static int base64_decode_strict(
    const char *in, size_t length, unsigned char **out, size_t *out_length) {
    if (length % 4u != 0u) return 0;
    size_t padding = 0u;
    if (length >= 1u && in[length - 1u] == '=') ++padding;
    if (length >= 2u && in[length - 2u] == '=') ++padding;
    /* Padding only at the very end; an '=' anywhere else is a rejection rather
     * than a byte to skip. */
    for (size_t index = 0u; index + padding < length; ++index) {
        if (in[index] == '=') return 0;
    }
    size_t capacity = (length / 4u) * 3u;
    unsigned char *buffer = malloc(capacity > 0u ? capacity : 1u);
    if (!buffer) return 0;
    size_t produced = 0u;
    for (size_t index = 0u; index < length; index += 4u) {
        int last = (index + 4u == length);
        int v0 = base64_value((unsigned char)in[index]);
        int v1 = base64_value((unsigned char)in[index + 1u]);
        if (v0 < 0 || v1 < 0) goto rejected;
        buffer[produced++] = (unsigned char)((v0 << 2) | (v1 >> 4));
        if (last && padding == 2u) {
            if ((v1 & 0x0F) != 0) goto rejected;
            break;
        }
        int v2 = base64_value((unsigned char)in[index + 2u]);
        if (v2 < 0) goto rejected;
        buffer[produced++] = (unsigned char)(((v1 & 0x0F) << 4) | (v2 >> 2));
        if (last && padding == 1u) {
            if ((v2 & 0x03) != 0) goto rejected;
            break;
        }
        int v3 = base64_value((unsigned char)in[index + 3u]);
        if (v3 < 0) goto rejected;
        buffer[produced++] = (unsigned char)(((v2 & 0x03) << 6) | v3);
    }
    *out = buffer;
    *out_length = produced;
    return 1;
rejected:
    free(buffer);
    return 0;
}

/*
 * Valid UTF-8 with no NUL. Stricter than the spec on purpose: the decoded value
 * is compared against a JSON string the runtime already refuses to accept with
 * an embedded NUL, so accepting one here could only produce a comparison that
 * can never succeed. Overlongs, surrogates and anything past U+10FFFF are
 * refused because each of them is a second spelling of a value that already has
 * one, and a comparison rule needs one spelling.
 */
static int utf8_valid_no_nul(const unsigned char *bytes, size_t length) {
    size_t index = 0u;
    while (index < length) {
        unsigned char lead = bytes[index];
        if (lead == 0x00u) return 0;
        if (lead < 0x80u) { ++index; continue; }
        size_t extra;
        unsigned int code;
        unsigned int minimum;
        if ((lead & 0xE0u) == 0xC0u) { extra = 1u; code = lead & 0x1Fu; minimum = 0x80u; }
        else if ((lead & 0xF0u) == 0xE0u) { extra = 2u; code = lead & 0x0Fu; minimum = 0x800u; }
        else if ((lead & 0xF8u) == 0xF0u) { extra = 3u; code = lead & 0x07u; minimum = 0x10000u; }
        else return 0;
        if (index + extra >= length) return 0;
        for (size_t step = 1u; step <= extra; ++step) {
            unsigned char continuation = bytes[index + step];
            if ((continuation & 0xC0u) != 0x80u) return 0;
            code = (code << 6) | (continuation & 0x3Fu);
        }
        if (code < minimum) return 0;
        if (code > 0x10FFFFu) return 0;
        if (code >= 0xD800u && code <= 0xDFFFu) return 0;
        index += extra + 1u;
    }
    return 1;
}

/*
 * Decode Mcp-Name if and only if it looks encoded. A value that starts with the
 * prefix AND ends with the suffix is decoded; anything else is taken literally.
 *
 * A literal value that itself matches the pattern must have been encoded by the
 * client. This function needs no rule for that case - it decodes what looks
 * encoded - and a mismatch that results is a genuine HeaderMismatch, reported
 * as one rather than as a decoding failure.
 *
 * Returns 1 on success. On a literal value *out stays NULL and the caller
 * compares the original bytes; on a decoded one *out is malloc'd.
 */
static int decode_mcp_name(
    const char *value, size_t length, unsigned char **out, size_t *out_length) {
    *out = NULL;
    *out_length = 0u;
    if (length < SENTINEL_MIN_BYTES) return 1;
    if (memcmp(value, SENTINEL_PREFIX, SENTINEL_PREFIX_BYTES) != 0) return 1;
    if (value[length - 2u] != '?' || value[length - 1u] != '=') return 1;
    unsigned char *decoded = NULL;
    size_t decoded_length = 0u;
    if (!base64_decode_strict(value + SENTINEL_PREFIX_BYTES,
            length - SENTINEL_MIN_BYTES, &decoded, &decoded_length)) {
        return 0;
    }
    if (!utf8_valid_no_nul(decoded, decoded_length)) {
        free(decoded);
        return 0;
    }
    *out = decoded;
    *out_length = decoded_length;
    return 1;
}

/* ------------------------------------------------------ body classification */

typedef enum body_kind {
    BODY_REQUEST = 0,
    BODY_NOTIFICATION = 1,
    BODY_RESPONSE = 2,
    BODY_INVALID = 3
} body_kind_t;

/*
 * The design's own words: a JSON-RPC response is the body that "has `id`, no
 * `method`". Nothing here inspects result-versus-error, because the refusal
 * does not depend on which of them it carries.
 *
 * An explicit `"id": null` alongside a method is BODY_INVALID rather than a
 * notification. JSON-RPC defines a notification as a request object WITHOUT an
 * id member, so treating a null id as one would invent a third spelling of
 * "notification" and make the Mcp-Method requirement depend on which spelling a
 * client picked.
 */
static body_kind_t classify_body(json_t *root) {
    json_t *method = json_object_get(root, "method");
    int has_id = json_object_get(root, "id") != NULL;
    int id_is_null = json_is_null(json_object_get(root, "id"));
    if (!method) return has_id ? BODY_RESPONSE : BODY_INVALID;
    if (!json_is_string(method)) return BODY_INVALID;
    if (!has_id) return BODY_NOTIFICATION;
    if (id_is_null) return BODY_INVALID;
    return BODY_REQUEST;
}

/* ------------------------------------------------------------- the failures */

typedef struct validation_failure {
    int status;
    int code;
    const char *message;
    /* Borrowed from the parsed body; copied into the response by
     * maelys_mcp_error_response, never shared with it. */
    json_t *id;
} validation_failure_t;

static maelys_mcp_result_t write_error(
    const maelys_mcp_http_response_writer_t *writer,
    const validation_failure_t *failure) {
    json_t *response = maelys_mcp_error_response(
        failure->id, failure->code, failure->message, NULL);
    if (!response) return MAELYS_MCP_ERR_MEMORY;
    char *body = json_dumps(response, JSON_COMPACT);
    json_decref(response);
    if (!body) return MAELYS_MCP_ERR_MEMORY;
    maelys_mcp_result_t status =
        writer->begin_json(writer->context, failure->status, body, strlen(body));
    free(body);
    return status;
}

/* --------------------------------------------------------- header validation */

static int header_present(
    const maelys_mcp_http_request_t *request,
    const char *name,
    const char **out_value,
    size_t *out_length) {
    /*
     * A header the embedder saw more than once is reported ABSENT by contract,
     * so this returns 0 for it and the caller refuses the request as missing
     * that header. That is the whole reason the duplicate rule is part of the
     * seam: "present once" has to be what a hit means, or the comparison below
     * would be validating one occurrence while dispatch executed another.
     */
    return request->header_lookup(
        request->header_context, name, out_value, out_length);
}

/*
 * Header values are compared case-SENSITIVELY. Header names are not - the
 * lookup does that - but `tools/call` and `Tools/Call` are different methods,
 * and folding them together here would make the transport disagree with the
 * dispatcher about what was asked for.
 */
static int slice_equals_json_string(
    const char *value, size_t length, json_t *node) {
    if (!json_is_string(node)) return 0;
    const char *text = json_string_value(node);
    size_t text_length = json_string_length(node);
    return length == text_length && memcmp(value, text, length) == 0;
}

static int bytes_equal_json_string(
    const unsigned char *value, size_t length, json_t *node) {
    return slice_equals_json_string((const char *)value, length, node);
}

/*
 * MCP-Protocol-Version. Mandatory, and validated for AGREEMENT with the body's
 * `_meta` version rather than for supportedness.
 *
 * The split is the status table's, not a shortcut. "unsupported protocol
 * version | 400 | -32022" names src/core/runtime.c as its producer, so an
 * adapter that also refused an unsupported-but-agreeing pair would take that
 * row away from the layer the table assigns it to. An agreeing pair naming a
 * version this transport does not serve therefore passes here and is refused at
 * dispatch in H3 - which is where the era mask already refuses it, and where
 * one refusal can serve both transports.
 *
 * A body with no `_meta` version at all fails as a mismatch, which is what
 * makes -32002 unreachable: the header is mandatory and cannot equal a value
 * that is not there.
 */
static int protocol_version_agrees(
    const maelys_mcp_http_request_t *request,
    json_t *root,
    const char **out_reason) {
    const char *value = NULL;
    size_t length = 0u;
    if (!header_present(request, "MCP-Protocol-Version", &value, &length)) {
        *out_reason = "MCP-Protocol-Version is required, exactly once";
        return 0;
    }
    json_t *params = json_object_get(root, "params");
    json_t *meta = json_is_object(params) ? json_object_get(params, "_meta") : NULL;
    json_t *version = json_is_object(meta)
        ? json_object_get(meta, "io.modelcontextprotocol/protocolVersion")
        : NULL;
    if (!version) {
        *out_reason =
            "MCP-Protocol-Version has no matching _meta protocol version in the body";
        return 0;
    }
    if (!slice_equals_json_string(value, length, version)) {
        *out_reason = "MCP-Protocol-Version disagrees with the body's _meta version";
        return 0;
    }
    return 1;
}

/*
 * Mcp-Method. Required on a JSON-RPC request; on a notification it is optional,
 * and compared whenever it is present.
 *
 * The design's inbound table says "yes on requests", and says nothing about a
 * notification. Requiring it there would refuse traffic the table does not
 * refuse; ignoring it when present would leave a header that disagrees with the
 * body unchecked, which is the exact hole the header/body rule exists to close.
 * Optional-but-checked is the only reading that adds no requirement and drops
 * no check. Recorded as a resolution rather than a reading.
 */
static int mcp_method_agrees(
    const maelys_mcp_http_request_t *request,
    json_t *root,
    body_kind_t kind,
    const char **out_reason) {
    const char *value = NULL;
    size_t length = 0u;
    if (!header_present(request, "Mcp-Method", &value, &length)) {
        if (kind == BODY_REQUEST) {
            *out_reason = "Mcp-Method is required on a request, exactly once";
            return 0;
        }
        return 1;
    }
    if (!slice_equals_json_string(value, length, json_object_get(root, "method"))) {
        *out_reason = "Mcp-Method disagrees with the body's method";
        return 0;
    }
    return 1;
}

/*
 * Mcp-Name. Required on tools/call and resources/read, and compared against
 * params.name / params.uri after sentinel decoding.
 *
 * For resources/read the comparison is against the RAW params.uri, never
 * against the canonicalized form the resources module later produces:
 * normalizing before comparing would let two different header values both pass,
 * which is the exact ambiguity this rule removes.
 *
 * On any other method there is no body field to compare against, so the header
 * is neither required nor compared. `prompts/get` is on the spec's list and
 * absent from this one because there is no Prompts module - it is a 404 with
 * -32601 at dispatch and never reaches a name check.
 */
static int mcp_name_agrees(
    const maelys_mcp_http_request_t *request,
    json_t *root,
    const char **out_reason) {
    json_t *method = json_object_get(root, "method");
    if (!json_is_string(method)) return 1;
    const char *method_text = json_string_value(method);
    int is_tools_call = strcmp(method_text, "tools/call") == 0;
    int is_resources_read = strcmp(method_text, "resources/read") == 0;
    if (!is_tools_call && !is_resources_read) return 1;

    const char *value = NULL;
    size_t length = 0u;
    if (!header_present(request, "Mcp-Name", &value, &length)) {
        *out_reason = is_tools_call
            ? "Mcp-Name is required on tools/call, exactly once"
            : "Mcp-Name is required on resources/read, exactly once";
        return 0;
    }
    unsigned char *decoded = NULL;
    size_t decoded_length = 0u;
    /* Decoding happens BEFORE comparison with the body, never after. */
    if (!decode_mcp_name(value, length, &decoded, &decoded_length)) {
        *out_reason = "Mcp-Name carries a malformed Base64 sentinel";
        return 0;
    }
    json_t *params = json_object_get(root, "params");
    json_t *target = json_is_object(params)
        ? json_object_get(params, is_tools_call ? "name" : "uri")
        : NULL;
    int agrees = decoded
        ? bytes_equal_json_string(decoded, decoded_length, target)
        : slice_equals_json_string(value, length, target);
    free(decoded);
    if (!agrees) {
        *out_reason = is_tools_call
            ? "Mcp-Name disagrees with the body's params.name"
            : "Mcp-Name disagrees with the body's params.uri";
        return 0;
    }
    return 1;
}

/*
 * The whole validation, in the order the design puts it in.
 *
 * Parse before classify before compare, because each step needs the one before
 * it: a body that is not JSON has no fields for a header to disagree with, so
 * -32700 must precede -32020 rather than compete with it.
 *
 * Returns 1 when the exchange may proceed. On 0, *failure is filled and its
 * `id` borrows from `root`, which the caller keeps alive across the write.
 */
static int validate_exchange(
    const maelys_mcp_http_request_t *request,
    json_t *root,
    validation_failure_t *failure) {
    body_kind_t kind = classify_body(root);
    if (kind == BODY_RESPONSE) {
        /*
         * Refused at the door rather than passed down. The spec forbids clients
         * from sending one and the modern era has no server-to-client requests
         * for it to be answering, so dispatching it could only produce an
         * Invalid Request into a body the client is not, by its own protocol,
         * supposed to be reading.
         */
        failure->status = 400;
        failure->code = JSONRPC_INVALID_REQUEST;
        failure->message = "A JSON-RPC response is not accepted on this transport";
        failure->id = NULL;
        return 0;
    }
    if (kind == BODY_INVALID) {
        failure->status = 400;
        failure->code = JSONRPC_INVALID_REQUEST;
        failure->message = "The body is not a JSON-RPC request or notification";
        failure->id = NULL;
        return 0;
    }

    /*
     * The id is carried on a -32020 and omitted on the two rows above it. The
     * status table marks "no id" on those two and does not mark it here, and
     * the asymmetry is earned: by the time a header can be compared the body has
     * parsed, so the id is known and a client can match the refusal to the call
     * it made. On the rows above, there is nothing trustworthy to echo.
     */
    json_t *id = json_object_get(root, "id");
    const char *reason = NULL;
    if (!protocol_version_agrees(request, root, &reason) ||
        !mcp_method_agrees(request, root, kind, &reason) ||
        !mcp_name_agrees(request, root, &reason)) {
        failure->status = 400;
        failure->code = MCP_HEADER_MISMATCH;
        failure->message = reason;
        failure->id = id;
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------- the exchange */

/* Raises the exchange's own wakeup descriptor. Caller holds `mutex`, and the
 * one-byte write is non-blocking, so a descriptor already carrying a byte is
 * left alone by EAGAIN rather than by a second flag. */
static void raise_wakeup_locked(maelys_mcp_http_exchange_t *exchange) {
    if (exchange->wake_write < 0) return;
    ssize_t written = write(exchange->wake_write, "x", 1u);
    (void)written;
}

void maelys_mcp_http_exchange_cancel(maelys_mcp_http_exchange_t *exchange) {
    if (!exchange) return;
    pthread_mutex_lock(&exchange->mutex);
    if (exchange->live) {
        exchange->cancelled = 1;
        raise_wakeup_locked(exchange);
    }
    pthread_mutex_unlock(&exchange->mutex);
}

void maelys_mcp_http_exchange_shutdown(maelys_mcp_http_exchange_t *exchange) {
    if (!exchange) return;
    pthread_mutex_lock(&exchange->mutex);
    /* A cancellation already recorded wins and is not downgraded: a peer that
     * is gone cannot be given a graceful ending. */
    if (exchange->live && !exchange->cancelled) {
        exchange->shutdown = 1;
        raise_wakeup_locked(exchange);
    }
    pthread_mutex_unlock(&exchange->mutex);
}

static void exchange_flags(
    maelys_mcp_http_exchange_t *exchange, int *out_cancelled, int *out_shutdown) {
    pthread_mutex_lock(&exchange->mutex);
    *out_cancelled = exchange->cancelled;
    *out_shutdown = exchange->shutdown;
    pthread_mutex_unlock(&exchange->mutex);
}

/*
 * Whether this exchange has already been cancelled, asked without waiting.
 *
 * It exists for the exchanges that have no drain loop to notice it in. A
 * notification produces its whole reply synchronously, so nothing would ever
 * consult the cancellation source - and a peer that has gone would still be
 * written to, which contradicts the one guarantee this transport makes without
 * qualification: after a cancellation, no further bytes are written for that
 * request. Uniform is worth more here than the byte it saves.
 */
static int already_cancelled(
    maelys_mcp_http_exchange_t *exchange, int cancel_fd) {
    int cancelled = 0;
    int shutdown = 0;
    exchange_flags(exchange, &cancelled, &shutdown);
    if (cancelled) return 1;
    int descriptors[2] = {cancel_fd, exchange->wake_read};
    for (size_t index = 0u; index < 2u; ++index) {
        if (descriptors[index] < 0) continue;
        struct pollfd probe = {
            .fd = descriptors[index], .events = POLLIN, .revents = 0};
        if (poll(&probe, 1u, 0) > 0 && probe.revents) return 1;
    }
    return 0;
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
    int read_fd = exchange->wake_read;
    int write_fd = exchange->wake_write;
    exchange->wake_read = -1;
    exchange->wake_write = -1;
    pthread_mutex_unlock(&exchange->mutex);
    /* Closed after `live` is cleared and under nobody's lock, so a canceller
     * that got in first wrote to a descriptor that was still open and one that
     * arrives later finds no descriptor to write to. */
    if (read_fd >= 0) close(read_fd);
    if (write_fd >= 0) close(write_fd);
    pthread_mutex_destroy(&exchange->mutex);
    free(exchange);
}

/*
 * The wakeup pipe, allocated only for an embedder that gave neither descriptor.
 * The shared helper is the one the stdio transport and the outbox already use,
 * so the close-on-exec and non-blocking flags cannot drift apart between three
 * copies of the same trick.
 *
 * Failure is not fatal to the exchange: the request is still served, and what
 * is lost is only the promptness of an out-of-band cancel. Refusing the whole
 * POST because a wakeup optimisation could not be allocated would be a worse
 * answer than serving it.
 */
static void open_wakeup(maelys_mcp_http_exchange_t *exchange) {
    int fds[2] = {-1, -1};
    if (maelys_mcp_create_wakeup_pipe(fds) != MAELYS_MCP_OK) return;
    exchange->wake_read = fds[0];
    exchange->wake_write = fds[1];
}

/* The one clock reading this file does. The graceful drain needs a remaining
 * budget rather than an expiry test, which is the one shape
 * maelys_mcp_monotonic_deadline_expired does not answer. */
static int monotonic_ms(uint64_t *out_value) {
    struct timespec now;
    if (!out_value || clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    *out_value = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
    return 0;
}

/* ------------------------------------------------------------- the drain */

typedef enum stream_mode {
    /* Nothing has been written and the reply's Content-Type is still open. */
    MODE_UNDECIDED = 0,
    MODE_JSON = 1,
    MODE_STREAM = 2
} stream_mode_t;

typedef enum wake_reason {
    WAKE_OUTPUT = 0,
    WAKE_CANCEL = 1,
    WAKE_SHUTDOWN = 2,
    WAKE_IDLE = 3,
    WAKE_ERROR = 4
} wake_reason_t;

/*
 * The timeout maelys_mcp_channel_next is asked for once poll() has already said
 * a frame is there. It is 1 rather than 0 because the outbox refuses a zero
 * timeout outright - a zero-length wait is not a thing that API expresses - and
 * it is the smallest value that means "take what is ready".
 *
 * This is NOT the rejected short-bounded-wait design. Nothing here waits on
 * this number for a frame's arrival: readiness is a descriptor, and this is
 * only the floor the take API imposes on the call that collects what readiness
 * already announced. The only way to spend it is a spurious wakeup.
 */
#define DRAIN_TAKE_TIMEOUT_MS 1u

typedef struct drain_state {
    const maelys_mcp_http_response_writer_t *writer;
    maelys_mcp_http_exchange_t *exchange;
    maelys_mcp_channel_t *channel;
    /* Borrowed from the parsed body and alive for the whole drain: it is what
     * "the final response for THIS request's id" is compared against. */
    json_t *id;
    stream_mode_t mode;
    unsigned int keepalive_interval_ms;
    /* Already resolved to a real duration by the caller, so nothing downstream
     * has to know that zero means "the channel default". */
    unsigned int close_timeout_ms;
    int cancel_fd;
    int shutdown_fd;
    /*
     * The adapter's own wakeup, present only when the embedder supplied neither
     * descriptor. It carries both dispositions and says which through the
     * exchange's flags rather than through which descriptor fired, because
     * there is only one of it.
     */
    int wake_fd;
} drain_state_t;

/*
 * Mode selection, and it is a question about the FRAME rather than about the
 * method: "is this the final response for this request's id?". A frame with a
 * method is a notification whatever else it carries, and the one that catches
 * readers out is subscriptions/listen's acknowledgement - it travels the
 * completion path even though it is a JSON-RPC notification with no id, so this
 * says no about it and the stream opens, which is correct.
 */
static int is_final_response(json_t *frame, json_t *id) {
    if (!json_is_object(frame)) return 0;
    if (json_object_get(frame, "method")) return 0;
    json_t *frame_id = json_object_get(frame, "id");
    if (!frame_id || !id) return 0;
    if (!json_equal(frame_id, id)) return 0;
    return json_object_get(frame, "result") != NULL ||
        json_object_get(frame, "error") != NULL;
}

/*
 * The status table's dispatch half, read off the response the runtime produced.
 *
 * Only in JSON mode, and that is sound rather than convenient: every code below
 * is produced before any module can have emitted a frame - version and metadata
 * validation, the initialization gate, method routing - so none of them can
 * arrive mid-stream, after the status line is already 200.
 *
 * The default is 200, and it is the load-bearing entry. -32003 is a policy
 * denial and stays 200 because it is a normal MCP exchange whose answer is a
 * refusal; HTTP 403 is reserved for refusing a principal BEFORE dispatch.
 * -32600 stays 200 for the same reason - the runtime saying "this request has
 * no meaning on this channel", which is what an `initialize` over HTTP gets, is
 * an answer and not a transport failure.
 */
static int status_for_response(json_t *frame) {
    json_t *error = json_object_get(frame, "error");
    if (!json_is_object(error)) return 200;
    json_t *code = json_object_get(error, "code");
    if (!json_is_integer(code)) return 200;
    switch ((int)json_integer_value(code)) {
        /* Lets a client tell a modern server that does not implement a method
         * from a legacy HTTP+SSE server that does not host this endpoint, which
         * is why the 404 must carry the JSON-RPC body rather than an empty one. */
        case JSONRPC_METHOD_NOT_FOUND: return 404;
        case MCP_UNSUPPORTED_VERSION: return 400;
        case MCP_MISSING_REQUIRED_CLIENT_CAPABILITY: return 400;
        /* Should be unreachable - the mandatory MCP-Protocol-Version header
         * cannot equal a body version that is not there, so the header/body
         * check catches it first - and mapped anyway, because "unreachable" is
         * a claim about two checks agreeing and a table that assumes agreement
         * hides the day they stop. */
        case MCP_SERVER_NOT_INITIALIZED: return 400;
        default: return 200;
    }
}

/* One frame onto the wire, opening the reply in whichever mode the frame
 * selects the first time it is called. */
static maelys_mcp_result_t write_frame(drain_state_t *state, json_t *frame) {
    char *text = json_dumps(frame, JSON_COMPACT);
    if (!text) return MAELYS_MCP_ERR_MEMORY;
    size_t length = strlen(text);
    maelys_mcp_result_t status;
    if (state->mode == MODE_UNDECIDED) {
        if (is_final_response(frame, state->id)) {
            state->mode = MODE_JSON;
            status = state->writer->begin_json(state->writer->context,
                status_for_response(frame), text, length);
            free(text);
            return status;
        }
        status = state->writer->begin_stream(state->writer->context);
        if (status != MAELYS_MCP_OK) {
            free(text);
            return status;
        }
        state->mode = MODE_STREAM;
    }
    status = state->writer->write_event(state->writer->context, text, length);
    free(text);
    return status;
}

/*
 * A0: simultaneously waiting for output and for the two ways this exchange can
 * be asked to end early.
 *
 * Priority is CANCEL, then SHUTDOWN, then OUTPUT, and it is deliberate. A frame
 * queued behind a disconnect must not be written on the way to noticing the
 * disconnect, which is what "no further writes after a client goes away" means
 * when it is a fact rather than an aspiration.
 */
static wake_reason_t wait_for_wake(drain_state_t *state) {
    for (;;) {
        int cancelled = 0;
        int shutdown = 0;
        exchange_flags(state->exchange, &cancelled, &shutdown);
        if (cancelled) return WAKE_CANCEL;
        struct pollfd descriptors[4];
        nfds_t count = 0u;
        int cancel_index = -1;
        int shutdown_index = -1;
        int wake_index = -1;
        int output_index = -1;
        int wait_fd = maelys_mcp_channel_wait_fd(state->channel);
        if (state->wake_fd >= 0) {
            wake_index = (int)count;
            descriptors[count].fd = state->wake_fd;
            descriptors[count].events = POLLIN;
            descriptors[count++].revents = 0;
        }
        if (state->cancel_fd >= 0) {
            cancel_index = (int)count;
            descriptors[count].fd = state->cancel_fd;
            descriptors[count].events = POLLIN;
            descriptors[count++].revents = 0;
        }
        if (state->shutdown_fd >= 0) {
            shutdown_index = (int)count;
            descriptors[count].fd = state->shutdown_fd;
            descriptors[count].events = POLLIN;
            descriptors[count++].revents = 0;
        }
        if (wait_fd >= 0) {
            output_index = (int)count;
            descriptors[count].fd = wait_fd;
            descriptors[count].events = POLLIN;
            descriptors[count++].revents = 0;
        }
        if (!count) return WAKE_ERROR;
        /* The shutdown flag is only actionable once nothing more urgent is
         * pending, so it is read here rather than above the poll. */
        if (shutdown) return WAKE_SHUTDOWN;
        int ready = poll(descriptors, count, (int)state->keepalive_interval_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return WAKE_ERROR;
        }
        if (ready == 0) return WAKE_IDLE;
        /* POLLERR and POLLHUP are readiness too: a cancellation source whose
         * writer went away has said what it had to say. */
        if (cancel_index >= 0 && descriptors[cancel_index].revents) return WAKE_CANCEL;
        if (shutdown_index >= 0 && descriptors[shutdown_index].revents) return WAKE_SHUTDOWN;
        if (wake_index >= 0 && descriptors[wake_index].revents) {
            /*
             * The adapter's own wakeup says only "look again". Which
             * disposition it carries is on the exchange, and the flag was set
             * under the same lock and before the byte was written, so the top
             * of this loop cannot see the byte without seeing the flag. A wake
             * with neither flag set is treated as an idle tick rather than as
             * an error, because spinning on it would be worse than one wasted
             * pass.
             */
            int now_cancelled = 0;
            int now_shutdown = 0;
            exchange_flags(state->exchange, &now_cancelled, &now_shutdown);
            if (now_cancelled) return WAKE_CANCEL;
            if (now_shutdown) return WAKE_SHUTDOWN;
            return WAKE_IDLE;
        }
        if (output_index >= 0 && descriptors[output_index].revents) return WAKE_OUTPUT;
        return WAKE_ERROR;
    }
}

/* Everything the channel still holds, thrown away rather than written. Called
 * once the exchange has produced its answer, so that the destroy below finds an
 * empty outbox and returns rather than spending its close deadline draining a
 * queue nobody will read. */
static void discard_remaining(maelys_mcp_channel_t *channel) {
    for (;;) {
        json_t *frame = NULL;
        if (maelys_mcp_channel_next(channel, DRAIN_TAKE_TIMEOUT_MS, &frame) !=
            MAELYS_MCP_OK) {
            return;
        }
        json_decref(frame);
    }
}

/*
 * The graceful ending, and the half of shutdown ordering that lives here.
 *
 * maelys_mcp_channel_close is deliberately NOT the call used, and the reason is
 * mechanical: it enqueues the completions and then waits for the outbox to
 * drain, on the same thread that is the only drainer, so calling it here would
 * spend the whole close deadline waiting for itself. The seam it uses instead
 * is the one channel_close uses - complete_subscriptions_until - after which
 * this loop is the drain channel_close would otherwise have waited for.
 */
static maelys_mcp_result_t drain_gracefully(drain_state_t *state) {
    uint64_t deadline = 0u;
    if (maelys_mcp_monotonic_deadline(state->close_timeout_ms, &deadline) != 0) {
        return MAELYS_MCP_ERR_IO;
    }
    (void)maelys_mcp_channel_complete_subscriptions_until(state->channel, deadline);
    maelys_mcp_result_t status = MAELYS_MCP_OK;
    int answered = 0;
    while (!answered) {
        uint64_t now = 0u;
        if (monotonic_ms(&now) != 0) break;
        if (now >= deadline) break;
        uint64_t remaining = deadline - now;
        if (remaining > 60000u) remaining = 60000u;
        json_t *frame = NULL;
        maelys_mcp_result_t next = maelys_mcp_channel_next(state->channel,
            (unsigned int)remaining, &frame);
        if (next != MAELYS_MCP_OK) break;
        answered = is_final_response(frame, state->id);
        status = write_frame(state, frame);
        json_decref(frame);
        if (status != MAELYS_MCP_OK) return status;
    }
    if (state->mode == MODE_STREAM) {
        /*
         * COMPLETE when the exchange got its ending, ABORTED when the deadline
         * passed without one. The disposition is the whole difference between
         * "this stream finished" and "this stream was cut", and telling a
         * client the first when the second happened is the one lie a truncated
         * response must not tell.
         */
        return state->writer->end_stream(state->writer->context, answered ?
            MAELYS_MCP_HTTP_STREAM_COMPLETE : MAELYS_MCP_HTTP_STREAM_ABORTED);
    }
    if (state->mode == MODE_UNDECIDED) {
        /* Nothing was ever written, so the reply is still choosable: the
         * runtime is going away and never answered, which is the status table's
         * 503 row. */
        validation_failure_t failure = {
            .status = 503,
            .code = JSONRPC_INVALID_REQUEST,
            .message = "The server is shutting down.",
            .id = NULL
        };
        return write_error(state->writer, &failure);
    }
    return status;
}

/*
 * A2-A6. The adapter's half of the cancellation chain, and it is the same code
 * whatever the cause was - a client FIN, an operator, a deadline - because by
 * the time it runs the only fact it has is that this exchange is cancelled.
 *
 * abort() faults the channel and closes its outbox with discard, which makes
 * sink->cancelled() answer 1 for a provider that polls its reporter and makes
 * every subsequent emit and complete fail ERR_CLOSED. "MUST NOT send any
 * further messages for it" therefore holds by construction rather than by
 * discipline. Whether the PROVIDER stops is the provider's own timeout, and
 * this does not shorten it.
 */
static maelys_mcp_result_t abandon(drain_state_t *state) {
    maelys_mcp_channel_abort(state->channel);
    if (state->mode == MODE_STREAM) {
        /* No terminal chunk. A chunked body that ends without one is exactly
         * how HTTP/1.1 says "this response is truncated". */
        (void)state->writer->end_stream(state->writer->context,
            MAELYS_MCP_HTTP_STREAM_ABORTED);
    }
    /* MODE_UNDECIDED writes nothing at all: no status line was chosen, and
     * inventing one for a peer that has gone away would be writing to a socket
     * for the sake of writing to it. */
    return MAELYS_MCP_ERR_CLOSED;
}

static maelys_mcp_result_t run_drain(drain_state_t *state) {
    for (;;) {
        wake_reason_t wake = wait_for_wake(state);
        if (wake == WAKE_CANCEL) return abandon(state);
        if (wake == WAKE_SHUTDOWN) return drain_gracefully(state);
        if (wake == WAKE_ERROR) return abandon(state);
        if (wake == WAKE_IDLE) {
            /* A bare ":" comment, and only once a stream exists - there is
             * nothing to keep alive before the reply's mode is chosen, and a
             * JSON reply has no place to put one. NULL write_keepalive is
             * legal and means "no keep-alives". */
            if (state->mode == MODE_STREAM && state->writer->write_keepalive) {
                maelys_mcp_result_t status =
                    state->writer->write_keepalive(state->writer->context);
                if (status != MAELYS_MCP_OK) return abandon(state);
            }
            continue;
        }
        json_t *frame = NULL;
        maelys_mcp_result_t next = maelys_mcp_channel_next(state->channel,
            DRAIN_TAKE_TIMEOUT_MS, &frame);
        if (next == MAELYS_MCP_ERR_TIMEOUT) {
            /* Spurious readability is possible and harmless; a missed wakeup is
             * not, which is why the flag is maintained under the queue's own
             * mutex rather than here. */
            continue;
        }
        if (next == MAELYS_MCP_ERR_CLOSED) {
            /*
             * Drained and closed without ever producing this request's
             * response: the channel faulted underneath the exchange. A stream
             * already opened is truncated, because that is what happened; a
             * reply not yet begun still gets a status, because the client is
             * still there and is owed one.
             */
            if (state->mode == MODE_STREAM) {
                (void)state->writer->end_stream(state->writer->context,
                    MAELYS_MCP_HTTP_STREAM_ABORTED);
                return MAELYS_MCP_ERR_CLOSED;
            }
            validation_failure_t failure = {
                .status = 503,
                .code = JSONRPC_INVALID_REQUEST,
                .message = "The request could not be completed.",
                .id = NULL
            };
            (void)write_error(state->writer, &failure);
            return MAELYS_MCP_ERR_CLOSED;
        }
        if (next != MAELYS_MCP_OK) return abandon(state);
        int final = is_final_response(frame, state->id);
        maelys_mcp_result_t status = write_frame(state, frame);
        json_decref(frame);
        if (status != MAELYS_MCP_OK) {
            /* A failed write is a peer that stopped reading, and it is
             * cancellation by another name: stop the channel, write nothing
             * further, and let the connection carry the truncation. */
            return abandon(state);
        }
        if (!final) continue;
        /* The final response terminates the stream: the body ends immediately
         * after it. */
        if (state->mode == MODE_STREAM) {
            return state->writer->end_stream(state->writer->context,
                MAELYS_MCP_HTTP_STREAM_COMPLETE);
        }
        return MAELYS_MCP_OK;
    }
}

/* ------------------------------------------------------ the principal bond */

/*
 * The thunk that turns the authenticator's refcount pair into the runtime's
 * context destructor. It exists because the two signatures differ in one
 * argument's type and casting a function pointer to call it through the wrong
 * prototype is undefined behaviour, not a formality.
 *
 * `context` is the principal itself and not this struct, deliberately:
 * maelys_mcp_channel_context is what middleware reads to learn who the runtime
 * is acting for, and handing it a wrapper would make every consumer unwrap one.
 */
typedef struct principal_binding {
    void (*release)(void *context, maelys_mcp_principal_t *principal);
    void *context;
} principal_binding_t;

static void release_principal(void *release_context, void *context) {
    principal_binding_t *binding = release_context;
    binding->release(binding->context, (maelys_mcp_principal_t *)context);
    free(binding);
}

/* ------------------------------------------------------------ the adapter */

maelys_mcp_result_t maelys_mcp_http_adapter_create(
    const maelys_mcp_http_adapter_config_t *config,
    maelys_mcp_http_adapter_t **out_adapter) {
    if (!out_adapter) return MAELYS_MCP_ERR_ARGUMENT;
    *out_adapter = NULL;
    /* A runtime is not optional any more. An adapter with none has nothing to
     * dispatch to, and the placeholder that used to stand in for one is gone. */
    if (!config || !config->runtime) return MAELYS_MCP_ERR_ARGUMENT;
    maelys_mcp_http_adapter_t *adapter = calloc(1u, sizeof(*adapter));
    if (!adapter) return MAELYS_MCP_ERR_MEMORY;
    adapter->runtime = config->runtime;
    adapter->channel_template.max_messages = config->max_messages;
    adapter->channel_template.max_bytes = config->max_bytes;
    adapter->channel_template.response_burst = config->response_burst;
    adapter->channel_template.admission_timeout_ms = config->admission_timeout_ms;
    adapter->channel_template.close_timeout_ms = config->close_timeout_ms;
    /*
     * The era mask, fixed for every channel this adapter will ever create, and
     * this is where the era rule becomes structural rather than a promise: a
     * channel that cannot be told to serve the legacy era cannot be talked into
     * it by any request, so `initialize` over HTTP is refused by the runtime and
     * not by a check the transport has to remember to run.
     */
    adapter->channel_template.protocol_eras = (unsigned int)MAELYS_MCP_ERA_MODERN;
    adapter->keepalive_interval_ms = config->keepalive_interval_ms
        ? config->keepalive_interval_ms : MAELYS_MCP_HTTP_KEEPALIVE_INTERVAL_MS;
    /* Resolved once, here, to the same default maelys_mcp_channel_create would
     * have applied, so the graceful drain never has to compute a deadline from
     * a zero that means something else. */
    adapter->close_timeout_ms = config->close_timeout_ms
        ? config->close_timeout_ms : MAELYS_MCP_HTTP_CLOSE_TIMEOUT_MS;
    *out_adapter = adapter;
    return MAELYS_MCP_OK;
}

void maelys_mcp_http_adapter_destroy(maelys_mcp_http_adapter_t *adapter) {
    free(adapter);
}

/*
 * channel_create with the principal and the era mask - the first real consumer
 * of the ABI 4 context destructor, and the reason it exists.
 *
 * Ownership transfers only on success, which is the config's own rule, so a
 * failed create leaves the retain this took to be undone here.
 */
static maelys_mcp_result_t create_channel(
    maelys_mcp_http_adapter_t *adapter,
    const maelys_mcp_http_request_t *request,
    maelys_mcp_channel_t **out_channel) {
    maelys_mcp_channel_config_t config = adapter->channel_template;
    config.context = request->principal;
    principal_binding_t *binding = NULL;
    if (request->principal && request->principal_retain && request->principal_release) {
        binding = calloc(1u, sizeof(*binding));
        if (!binding) return MAELYS_MCP_ERR_MEMORY;
        binding->release = request->principal_release;
        binding->context = request->principal_context;
        request->principal_retain(request->principal_context, request->principal);
        config.context_release = release_principal;
        config.release_context = binding;
    }
    maelys_mcp_result_t status =
        maelys_mcp_channel_create(adapter->runtime, &config, out_channel);
    if (status != MAELYS_MCP_OK && binding) {
        request->principal_release(request->principal_context, request->principal);
        free(binding);
    }
    return status;
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
    /* The lookup is how the adapter reads every header it reads. Without one
     * there is no way to validate, and validating nothing is not an option any
     * more. */
    if (!request->header_lookup) return MAELYS_MCP_ERR_ARGUMENT;
    /* Both or neither. One of the two would be a refcount with only one side,
     * which is a leak or a double free depending on which one was supplied. */
    if ((request->principal_retain != NULL) != (request->principal_release != NULL)) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    maelys_mcp_http_exchange_t *exchange = calloc(1u, sizeof(*exchange));
    if (!exchange) return MAELYS_MCP_ERR_MEMORY;
    if (pthread_mutex_init(&exchange->mutex, NULL) != 0) {
        free(exchange);
        return MAELYS_MCP_ERR_MEMORY;
    }
    exchange->live = 1;
    exchange->wake_read = -1;
    exchange->wake_write = -1;
    if (out_exchange && request->cancel_fd < 0 && request->shutdown_fd < 0) {
        open_wakeup(exchange);
    }
    /* Published before any work, which is the whole reason the argument
     * exists - an out-of-band canceller has to have something to name before
     * the thing it wants to cancel has started. */
    if (out_exchange) *out_exchange = exchange;

    maelys_mcp_result_t status;
    json_error_t parse_error;
    /*
     * json_loadb, not json_loads: the body is a view into the server layer's
     * read buffer and is not NUL-terminated.
     *
     * No JSON_REJECT_DUPLICATES. Jansson keeps the last occurrence of a
     * repeated key deterministically, and the object compared here is the same
     * object dispatched below, so a duplicate key cannot make the comparison
     * and the dispatch disagree - which is the only property this check owes.
     * Refusing it would be a new rule the design does not state.
     */
    json_t *root = request->body
        ? json_loadb((const char *)request->body, request->body_length, 0, &parse_error)
        : NULL;
    if (!root || !json_is_object(root)) {
        /* One row, two causes: not valid JSON, or valid JSON that is not an
         * object. The design gives them the same status and the same code, so
         * they are not told apart here either. */
        validation_failure_t failure = {
            .status = 400,
            .code = -32700,
            .message = "The request body is not a JSON object",
            .id = NULL
        };
        if (root) json_decref(root);
        status = write_error(writer, &failure);
        retire_exchange(exchange, out_exchange);
        return status;
    }

    validation_failure_t failure = {0};
    if (!validate_exchange(request, root, &failure)) {
        status = write_error(writer, &failure);
        json_decref(root);
        retire_exchange(exchange, out_exchange);
        return status;
    }

    maelys_mcp_channel_t *channel = NULL;
    status = create_channel(adapter, request, &channel);
    if (status != MAELYS_MCP_OK) {
        /*
         * Runtime destruction closes and drains the channel-create gate before
         * taking lifecycle_mutex, so a connection thread racing a shutdown gets
         * ERR_STATE here rather than a torn runtime. The status table maps that
         * to 503 - the one status in this design that is not in the spec's
         * table and is right anyway.
         */
        validation_failure_t refusal = {
            .status = 503,
            .code = JSONRPC_INVALID_REQUEST,
            .message = "The runtime cannot accept this request.",
            .id = NULL
        };
        status = write_error(writer, &refusal);
        json_decref(root);
        retire_exchange(exchange, out_exchange);
        return status;
    }
    /*
     * Before anything is dispatched, because the descriptor is what the drain
     * loop watches and a frame that arrived before it existed would have raised
     * nothing. Failure is answered 503 and is explicitly NOT answered by
     * falling back to short timed waits: that is the polling design this seam
     * exists to avoid, and silently degrading into it under descriptor pressure
     * would be worse than failing, because the failure is visible and the
     * degradation would not be.
     */
    if (maelys_mcp_channel_enable_wait_fd(channel) != MAELYS_MCP_OK) {
        validation_failure_t refusal = {
            .status = 503,
            .code = JSONRPC_INVALID_REQUEST,
            .message = "The server cannot serve this request.",
            .id = NULL
        };
        status = write_error(writer, &refusal);
        (void)maelys_mcp_channel_destroy_detached(channel);
        json_decref(root);
        retire_exchange(exchange, out_exchange);
        return status;
    }

    json_t *id = json_object_get(root, "id");
    maelys_mcp_result_t accepted = maelys_mcp_channel_accept(channel, root);
    if (!id) {
        /*
         * A notification: 202 Accepted with an empty body, and nothing waited
         * for. It is dispatched rather than dropped at the door, because the
         * runtime has its own rules for the ones it recognizes - it handles
         * notifications/cancelled, records notifications/initialized, and drops
         * the rest - and a transport that answered 202 without dispatching
         * would be deciding on the runtime's behalf which notifications matter.
         */
        (void)accepted;
        discard_remaining(channel);
        (void)maelys_mcp_channel_destroy_detached(channel);
        json_decref(root);
        /*
         * Dispatched either way, because the runtime's rules about which
         * notifications matter are the runtime's; ANSWERED only if there is
         * still somebody to answer. A cancelled exchange writes nothing, and
         * that holds for the one reply shape that never enters the drain loop
         * as much as for the ones that do.
         */
        status = already_cancelled(exchange, request->cancel_fd)
            ? MAELYS_MCP_ERR_CLOSED
            : writer->status_only(writer->context, 202, NULL, 0u);
        retire_exchange(exchange, out_exchange);
        return status;
    }

    drain_state_t state = {
        .writer = writer,
        .exchange = exchange,
        .channel = channel,
        .id = id,
        .mode = MODE_UNDECIDED,
        .keepalive_interval_ms = adapter->keepalive_interval_ms,
        .close_timeout_ms = adapter->close_timeout_ms,
        .cancel_fd = request->cancel_fd,
        .shutdown_fd = request->shutdown_fd,
        /* Read once here, and never after retire_exchange has closed it. */
        .wake_fd = exchange->wake_read
    };
    if (accepted != MAELYS_MCP_OK) {
        /* The frame never reached dispatch, so no response is coming and
         * waiting for one would wait forever. */
        validation_failure_t refusal = {
            .status = 503,
            .code = JSONRPC_INVALID_REQUEST,
            .message = "The request could not be accepted.",
            .id = NULL
        };
        status = write_error(writer, &refusal);
    } else {
        status = run_drain(&state);
    }
    /*
     * A7-A8. Detached, never plain destroy, and this is where the guarantee is
     * earned: maelys_mcp_channel_destroy answers a close that missed its
     * deadline by waiting however long the provider takes, which would make the
     * connection slot hostage to a wedged in-process call. Detaching converts
     * that into "the channel outlives the connection and is freed by its last
     * worker" - and context_release, which is where this exchange's principal
     * reference goes back, runs at that real free rather than here. The
     * unbounded wait does not disappear; it stops being something a network
     * peer can hold.
     */
    discard_remaining(channel);
    (void)maelys_mcp_channel_destroy_detached(channel);
    json_decref(root);
    retire_exchange(exchange, out_exchange);
    return status;
}
