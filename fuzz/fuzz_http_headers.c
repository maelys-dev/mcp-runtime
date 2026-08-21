/*
 * http-headers: a synthesized header block and body, into name normalization,
 * sentinel decoding and the header/body comparison.
 *
 * The entry point is maelys_mcp_http_adapter_handle itself rather than a
 * private helper, which is the payoff of the adapter seam being a function: a
 * whole validation pass costs no socket, no channel and no thread.
 *
 * What is asserted is not "the decoder agrees with itself". Three of these are
 * properties re-derived here, with an encoder written independently of the
 * decoder under test:
 *
 *  - a value this file encodes from the body's own name must be ACCEPTED, which
 *    is what a decoder that rejects valid input or mis-decodes would fail;
 *  - a value encoded from a name that differs from the body's by one byte must
 *    be REFUSED, which is the "never accept a mismatch" invariant;
 *  - a name that itself matches the sentinel pattern is carried through both of
 *    the above, because "mis-decode a value containing the sentinel pattern" is
 *    the collision case the design calls out by name.
 *
 * And over everything, the shape invariant: whatever the bytes, the writer is
 * driven exactly once in JSON mode and the status is one the design allows.
 */
#include "maelys/mcp/http.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define MAX_NAME 192u
#define MAX_ENCODED ((MAX_NAME + 2u) / 3u * 4u + 1u)
#define MAX_HEADER (MAX_ENCODED + 16u)
#define MAX_BODY 1024u

/* ------------------------------------------------------- recording writer */

typedef struct recorder {
    int begin_json_calls;
    int begin_stream_calls;
    int status_only_calls;
    int end_stream_calls;
    int status;
} recorder_t;

static maelys_mcp_result_t on_begin_json(
    void *context, int status, const char *body, size_t length) {
    recorder_t *recorder = context;
    (void)body;
    (void)length;
    ++recorder->begin_json_calls;
    recorder->status = status;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t on_begin_stream(void *context) {
    recorder_t *recorder = context;
    ++recorder->begin_stream_calls;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t on_event(void *context, const char *json, size_t length) {
    (void)context; (void)json; (void)length;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t on_keepalive(void *context) {
    (void)context;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t on_end_stream(
    void *context, maelys_mcp_http_stream_end_t disposition) {
    recorder_t *recorder = context;
    (void)disposition;
    ++recorder->end_stream_calls;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t on_status_only(
    void *context, int status, const char *const *extra, size_t extra_count) {
    recorder_t *recorder = context;
    (void)extra; (void)extra_count;
    ++recorder->status_only_calls;
    recorder->status = status;
    return MAELYS_MCP_OK;
}

/* --------------------------------------------------------- header lookup */

typedef struct headers {
    const char *version;
    const char *method;
    const char *name;
} headers_t;

static int lookup(
    void *context, const char *field, const char **out_value, size_t *out_length) {
    const headers_t *headers = context;
    const char *value = NULL;
    if (strcasecmp(field, "MCP-Protocol-Version") == 0) value = headers->version;
    else if (strcasecmp(field, "Mcp-Method") == 0) value = headers->method;
    else if (strcasecmp(field, "Mcp-Name") == 0) value = headers->name;
    if (!value) return 0;
    if (out_value) *out_value = value;
    if (out_length) *out_length = strlen(value);
    return 1;
}

/* ------------------------------------ a Base64 encoder, written from scratch */

static const char ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t encode_base64(const unsigned char *in, size_t length, char *out) {
    size_t produced = 0u;
    size_t index = 0u;
    while (index + 3u <= length) {
        unsigned int group = ((unsigned int)in[index] << 16) |
            ((unsigned int)in[index + 1u] << 8) | (unsigned int)in[index + 2u];
        out[produced++] = ALPHABET[(group >> 18) & 0x3Fu];
        out[produced++] = ALPHABET[(group >> 12) & 0x3Fu];
        out[produced++] = ALPHABET[(group >> 6) & 0x3Fu];
        out[produced++] = ALPHABET[group & 0x3Fu];
        index += 3u;
    }
    size_t remaining = length - index;
    if (remaining == 1u) {
        unsigned int group = (unsigned int)in[index] << 16;
        out[produced++] = ALPHABET[(group >> 18) & 0x3Fu];
        out[produced++] = ALPHABET[(group >> 12) & 0x3Fu];
        out[produced++] = '=';
        out[produced++] = '=';
    } else if (remaining == 2u) {
        unsigned int group = ((unsigned int)in[index] << 16) |
            ((unsigned int)in[index + 1u] << 8);
        out[produced++] = ALPHABET[(group >> 18) & 0x3Fu];
        out[produced++] = ALPHABET[(group >> 12) & 0x3Fu];
        out[produced++] = ALPHABET[(group >> 6) & 0x3Fu];
        out[produced++] = '=';
    }
    out[produced] = '\0';
    return produced;
}

/* ------------------------------------------------------------------ driver */

/*
 * Printable ASCII only, so the generated name is valid UTF-8 with no NUL by
 * construction. That is what lets the round-trip case assert ACCEPTANCE rather
 * than merely "did not crash": a name the decoder is entitled to refuse would
 * make the assertion wrong rather than the decoder.
 */
static size_t build_name(const uint8_t *data, size_t size, char *out) {
    size_t length = size < MAX_NAME ? size : MAX_NAME;
    for (size_t index = 0u; index < length; ++index) {
        out[index] = (char)(0x20 + (data[index] % 95u));
    }
    out[length] = '\0';
    return length;
}

/* Escapes only what printable ASCII can require. */
static void build_body(const char *name, char *out, size_t capacity) {
    char escaped[MAX_NAME * 2u + 1u];
    size_t produced = 0u;
    for (size_t index = 0u; name[index]; ++index) {
        if (name[index] == '"' || name[index] == '\\') escaped[produced++] = '\\';
        escaped[produced++] = name[index];
    }
    escaped[produced] = '\0';
    snprintf(out, capacity,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"%s\",\"_meta\":"
        "{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"}}}",
        escaped);
}

static int looks_like_sentinel(const char *value, size_t length) {
    return length >= 11u && memcmp(value, "=?base64?", 9u) == 0 &&
        value[length - 2u] == '?' && value[length - 1u] == '=';
}

static int drive(const char *header_name, const char *body, int *out_status) {
    maelys_mcp_http_adapter_t *adapter = NULL;
    if (maelys_mcp_http_adapter_create(
            MAELYS_MCP_HTTP_PLACEHOLDER_JSON, &adapter) != MAELYS_MCP_OK) {
        return 0;
    }
    headers_t headers = {
        .version = "2026-07-28",
        .method = "tools/call",
        .name = header_name
    };
    maelys_mcp_http_request_t request = {
        .method = "POST",
        .path = "/mcp",
        .header_lookup = lookup,
        .header_context = &headers,
        .body = body,
        .body_length = strlen(body),
        .principal = NULL,
        .cancel_fd = -1
    };
    recorder_t recorder = {0};
    maelys_mcp_http_response_writer_t writer = {
        .context = &recorder,
        .begin_json = on_begin_json,
        .begin_stream = on_begin_stream,
        .write_event = on_event,
        .write_keepalive = on_keepalive,
        .end_stream = on_end_stream,
        .status_only = on_status_only
    };
    maelys_mcp_result_t result =
        maelys_mcp_http_adapter_handle(adapter, &request, &writer, NULL);
    maelys_mcp_http_adapter_destroy(adapter);
    assert(result == MAELYS_MCP_OK);
    /* The shape invariant, for every input without exception. */
    assert(recorder.begin_json_calls == 1);
    assert(recorder.begin_stream_calls == 0);
    assert(recorder.status_only_calls == 0);
    assert(recorder.end_stream_calls == 0);
    /* 400 is a refusal; 503 is the H2 placeholder for a request that passed
     * validation. Nothing else is reachable from this layer. */
    assert(recorder.status == 400 || recorder.status == 503);
    *out_status = recorder.status;
    return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2u) return 0;
    unsigned int mode = data[0] % 4u;
    const uint8_t *payload = data + 1u;
    size_t payload_size = size - 1u;

    char name[MAX_NAME + 1u];
    size_t name_length = build_name(payload, payload_size, name);
    char body[MAX_BODY];
    build_body(name, body, sizeof(body));

    char header[MAX_HEADER];
    int status = 0;

    if (mode == 0u) {
        /* The literal value. It matches the body byte for byte, so it must be
         * accepted - UNLESS it happens to match the sentinel pattern, in which
         * case the design says it is decoded rather than taken literally and
         * the outcome depends on what it decodes to. Only the shape invariant
         * is claimed there. */
        if (name_length >= sizeof(header)) return 0;
        memcpy(header, name, name_length + 1u);
        if (!drive(header, body, &status)) return 0;
        if (!looks_like_sentinel(name, name_length)) {
            assert(status == 503);
        }
        return 0;
    }

    if (mode == 1u) {
        /* Round trip: encoded from the body's own name, so it must be accepted.
         * This holds for a name that itself matches the sentinel pattern, which
         * is exactly what a client with such a name is required to do. */
        char encoded[MAX_ENCODED];
        encode_base64((const unsigned char *)name, name_length, encoded);
        int written = snprintf(header, sizeof(header), "=?base64?%s?=", encoded);
        if (written < 0 || (size_t)written >= sizeof(header)) return 0;
        if (!drive(header, body, &status)) return 0;
        assert(status == 503);
        return 0;
    }

    if (mode == 2u) {
        /* One byte different from the body's name: never accepted. */
        char altered[MAX_NAME + 2u];
        memcpy(altered, name, name_length);
        altered[name_length] = 'x';
        altered[name_length + 1u] = '\0';
        char encoded[MAX_ENCODED + 4u];
        encode_base64((const unsigned char *)altered, name_length + 1u, encoded);
        int written = snprintf(header, sizeof(header), "=?base64?%s?=", encoded);
        if (written < 0 || (size_t)written >= sizeof(header)) return 0;
        if (!drive(header, body, &status)) return 0;
        assert(status == 400);
        return 0;
    }

    /* mode 3: arbitrary bytes inside the sentinel envelope - truncated payloads,
     * over-padding, non-alphabet bytes, embedded padding. Shape invariant only;
     * what this target is looking for here is a crash or an over-read. */
    char raw[MAX_NAME + 1u];
    size_t raw_length = payload_size < MAX_NAME ? payload_size : MAX_NAME;
    for (size_t index = 0u; index < raw_length; ++index) {
        char byte = (char)payload[index];
        /* NUL would terminate the header value early and test nothing. */
        raw[index] = byte ? byte : 'A';
    }
    raw[raw_length] = '\0';
    int written = snprintf(header, sizeof(header), "=?base64?%s?=", raw);
    if (written < 0 || (size_t)written >= sizeof(header)) return 0;
    drive(header, body, &status);
    return 0;
}
