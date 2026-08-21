/*
 * The HTTP <-> MCP adapter, H2 shape.
 *
 * H1 built both sides of the seam in include/maelys/mcp/http.h and answered
 * every exchange from a table. H2 puts the MCP validation in front of that
 * table: the routing headers are decoded and compared against the body, and a
 * disagreement is refused here rather than carried into a dispatch that would
 * execute the body while a gateway routed on the header.
 *
 * H2 STILL DOES NOT DISPATCH. "Still no dispatch" is the phase's own words
 * (docs/http-transport-design.md, H2), so a request that survives validation
 * falls through to H1's placeholder exactly as before. channel_create,
 * channel_accept, mode selection on the first frame, SSE framing, 202 for
 * notifications and the cancellation chain are H3.
 *
 * WHAT THIS FILE MAY PRODUCE, and why the list is exactly three rows long. The
 * design's status table carries a "Produced by" column, and calls it "the
 * layering made checkable" rather than decoration. Three of its rows name the
 * adapter:
 *
 *   body not valid JSON / not a JSON object   400  -32700
 *   body is a JSON-RPC response               400  -32600, no id
 *   header missing, malformed, or != body     400  -32020  HeaderMismatch
 *
 * Everything above those rows is the server layer's or the authenticator's and
 * landed in H1. Everything below them - -32022, -32601, -32021, -32002, -32003
 * - is "a dispatch result the transport reports and never rewrites", so it can
 * only be produced once there is a dispatch, in H3. This file producing a
 * fourth row would be this file lying about which layer decided.
 *
 * What is real here and must stay real is the shape: no descriptor is touched,
 * no socket header is included, the exchange handle is published before any
 * work and retired before _handle returns, and the writer is the only way a
 * byte leaves.
 */
#include "src/internal/internal.h"

#include "maelys/mcp/http.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/*
 * The one placeholder body. 503 with -32600 and no id is the status table's
 * shape for "the runtime cannot serve this request", which is exactly true
 * here: the endpoint parses, authenticates and now validates, and there is no
 * dispatch behind it until H3. Answering 200 with a synthetic result would be
 * the one thing a placeholder must not do.
 */
static const char PLACEHOLDER_JSON[] =
    "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,"
    "\"message\":\"The HTTP transport does not serve MCP yet.\"}}";

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

/* --------------------------------------------------------------- Base64 */

static int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
    if (c >= 'a' && c <= 'z') return (int)(c - 'a') + 26;
    if (c >= '0' && c <= '9') return (int)(c - '0') + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    /* No URL-safe alphabet: '-' and '_' are not accepted, and neither is any
     * whitespace, so there is no line wrapping either. */
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

/* ------------------------------------------------------------ the adapter */

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
    /* The lookup is how the adapter reads every header it reads. Without one
     * there is no way to validate, and validating nothing is not an option any
     * more. */
    if (!request->header_lookup) return MAELYS_MCP_ERR_ARGUMENT;
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
    json_error_t parse_error;
    /*
     * json_loadb, not json_loads: the body is a view into the server layer's
     * read buffer and is not NUL-terminated.
     *
     * No JSON_REJECT_DUPLICATES. Jansson keeps the last occurrence of a
     * repeated key deterministically, and the object compared here is the same
     * object H3 will dispatch, so a duplicate key cannot make the comparison
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
    json_decref(root);

    /*
     * Validation passed, and H2 stops here. The placeholder answers, exactly as
     * it did in H1, because "Still no dispatch" is this phase's own boundary and
     * H3 is what replaces the table with mode selection on the first frame the
     * channel produces.
     */
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
