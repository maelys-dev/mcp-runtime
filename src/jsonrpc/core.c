/*
 * Extracted from codexmanager/transports/jsonrpc_transport_core.c and adapted
 * to the independent Maelys MCP result and configuration types.
 */
#include "src/jsonrpc/core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

#define INITIAL_BUFFER_BYTES 4096u
#define DEFAULT_HEADER_BYTES 8192u
#define DEFAULT_BODY_BYTES (1024u * 1024u)

static int append_bytes(maelys_mcp_jsonrpc_core_t *core, const char *bytes, size_t length) {
    if (!core || (!bytes && length)) return -1;
    if (!length) return 0;
    if (length > (size_t)-1 - core->length) return -1;
    size_t required = core->length + length;
    if (required > core->capacity) {
        size_t capacity = core->capacity ? core->capacity : INITIAL_BUFFER_BYTES;
        while (capacity < required) {
            if (capacity > (size_t)-1 / 2u) return -1;
            capacity *= 2u;
        }
        char *grown = realloc(core->buffer, capacity);
        if (!grown) return -1;
        core->buffer = grown;
        core->capacity = capacity;
    }
    memcpy(core->buffer + core->length, bytes, length);
    core->length += length;
    return 0;
}

static void consume_bytes(maelys_mcp_jsonrpc_core_t *core, size_t length) {
    if (!core || !length) return;
    if (length >= core->length) { core->length = 0; return; }
    memmove(core->buffer, core->buffer + length, core->length - length);
    core->length -= length;
}

static ssize_t find_line_end(const char *bytes, size_t length) {
    for (size_t index = 0; bytes && index < length; ++index) {
        if (bytes[index] == '\n') return (ssize_t)index;
    }
    return -1;
}

static ssize_t find_header_end(const char *bytes, size_t length) {
    if (!bytes || length < 4) return -1;
    for (size_t index = 0; index + 3u < length; ++index) {
        if (bytes[index] == '\r' && bytes[index + 1u] == '\n' &&
            bytes[index + 2u] == '\r' && bytes[index + 3u] == '\n') return (ssize_t)index;
    }
    return -1;
}

static int parse_length_value(const char *value, size_t length, size_t *out) {
    while (length && (*value == ' ' || *value == '\t')) { ++value; --length; }
    if (!length) return -1;
    size_t result = 0;
    for (size_t index = 0; index < length; ++index) {
        unsigned char byte = (unsigned char)value[index];
        if (byte == ' ' || byte == '\t') {
            for (size_t tail = index; tail < length; ++tail) {
                if (value[tail] != ' ' && value[tail] != '\t') return -1;
            }
            *out = result;
            return 0;
        }
        if (byte < '0' || byte > '9') return -1;
        size_t digit = (size_t)(byte - '0');
        if (result > ((size_t)-1 - digit) / 10u) return -1;
        result = result * 10u + digit;
    }
    *out = result;
    return 0;
}

static int parse_content_length(const char *header, size_t length, size_t *out) {
    static const char prefix[] = "Content-Length:";
    size_t position = 0;
    int found = 0;
    while (position < length) {
        size_t start = position;
        while (position + 1u < length && !(header[position] == '\r' && header[position + 1u] == '\n')) ++position;
        if (position + 1u >= length) position = length;
        size_t line_length = position - start;
        position = position + 1u < length ? position + 2u : length;
        if (line_length >= sizeof(prefix) - 1u &&
            strncasecmp(header + start, prefix, sizeof(prefix) - 1u) == 0) {
            if (found || parse_length_value(header + start + sizeof(prefix) - 1u,
                line_length - sizeof(prefix) + 1u, out) != 0) return -1;
            found = 1;
        }
    }
    return found ? 0 : -1;
}

static maelys_mcp_result_t emit(maelys_mcp_jsonrpc_core_t *core, json_t *message) {
    if (!core->on_message) { json_decref(message); return MAELYS_MCP_ERR_ARGUMENT; }
    return core->on_message(core->context, message);
}

static maelys_mcp_result_t parse_lines(maelys_mcp_jsonrpc_core_t *core) {
    for (;;) {
        ssize_t end = find_line_end(core->buffer, core->length);
        if (end < 0) return core->length > core->max_line_bytes ? MAELYS_MCP_ERR_PROTOCOL : MAELYS_MCP_OK;
        size_t raw_length = (size_t)end;
        size_t line_length = raw_length;
        if (line_length && core->buffer[line_length - 1u] == '\r') --line_length;
        if (!line_length || line_length > core->max_line_bytes) return MAELYS_MCP_ERR_PROTOCOL;
        json_error_t error;
        json_t *message = json_loadb(core->buffer, line_length, JSON_REJECT_DUPLICATES, &error);
        consume_bytes(core, raw_length + 1u);
        if (!message) return MAELYS_MCP_ERR_PROTOCOL;
        maelys_mcp_result_t status = emit(core, message);
        if (status != MAELYS_MCP_OK) return status;
    }
}

static maelys_mcp_result_t parse_frames(maelys_mcp_jsonrpc_core_t *core) {
    for (;;) {
        ssize_t header_end = find_header_end(core->buffer, core->length);
        if (header_end < 0) return core->length > core->max_header_bytes ? MAELYS_MCP_ERR_PROTOCOL : MAELYS_MCP_OK;
        size_t header_length = (size_t)header_end;
        if (header_length > core->max_header_bytes) return MAELYS_MCP_ERR_PROTOCOL;
        size_t body_length = 0;
        if (parse_content_length(core->buffer, header_length, &body_length) != 0 ||
            body_length > core->max_body_bytes || body_length > (size_t)-1 - header_length - 4u) {
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        size_t frame_length = header_length + 4u + body_length;
        if (core->length < frame_length) return MAELYS_MCP_OK;
        json_error_t error;
        json_t *message = json_loadb(core->buffer + header_length + 4u,
            body_length, JSON_REJECT_DUPLICATES, &error);
        consume_bytes(core, frame_length);
        if (!message) return MAELYS_MCP_ERR_PROTOCOL;
        maelys_mcp_result_t status = emit(core, message);
        if (status != MAELYS_MCP_OK) return status;
    }
}

maelys_mcp_result_t maelys_mcp_jsonrpc_core_init(
    maelys_mcp_jsonrpc_core_t *core,
    const maelys_mcp_jsonrpc_options_t *options) {
    if (!core || !options || !options->on_message) return MAELYS_MCP_ERR_ARGUMENT;
    if (options->framing != MAELYS_MCP_JSONRPC_CONTENT_LENGTH &&
        options->framing != MAELYS_MCP_JSONRPC_JSON_LINES) return MAELYS_MCP_ERR_ARGUMENT;
    memset(core, 0, sizeof(*core));
    core->framing = options->framing;
    core->max_header_bytes = options->max_header_bytes ? options->max_header_bytes : DEFAULT_HEADER_BYTES;
    core->max_body_bytes = options->max_body_bytes ? options->max_body_bytes : DEFAULT_BODY_BYTES;
    core->max_line_bytes = options->max_line_bytes ? options->max_line_bytes : DEFAULT_BODY_BYTES;
    core->on_message = options->on_message;
    core->context = options->context;
    return MAELYS_MCP_OK;
}

void maelys_mcp_jsonrpc_core_clear(maelys_mcp_jsonrpc_core_t *core) {
    if (!core) return;
    free(core->buffer);
    memset(core, 0, sizeof(*core));
}

maelys_mcp_result_t maelys_mcp_jsonrpc_core_feed(
    maelys_mcp_jsonrpc_core_t *core,
    const void *bytes,
    size_t length) {
    if (!core || (!bytes && length)) return MAELYS_MCP_ERR_ARGUMENT;
    if (append_bytes(core, bytes, length) != 0) return MAELYS_MCP_ERR_MEMORY;
    return core->framing == MAELYS_MCP_JSONRPC_JSON_LINES ? parse_lines(core) : parse_frames(core);
}

maelys_mcp_result_t maelys_mcp_jsonrpc_core_finish(
    const maelys_mcp_jsonrpc_core_t *core) {
    if (!core) return MAELYS_MCP_ERR_ARGUMENT;
    return core->length == 0 ? MAELYS_MCP_OK : MAELYS_MCP_ERR_PROTOCOL;
}

maelys_mcp_result_t maelys_mcp_jsonrpc_core_serialize(
    const maelys_mcp_jsonrpc_core_t *core,
    json_t *message,
    char **out_bytes,
    size_t *out_length) {
    if (!core || !message || !out_bytes || !out_length) return MAELYS_MCP_ERR_ARGUMENT;
    if (core->framing != MAELYS_MCP_JSONRPC_JSON_LINES &&
        core->framing != MAELYS_MCP_JSONRPC_CONTENT_LENGTH) return MAELYS_MCP_ERR_ARGUMENT;
    *out_bytes = NULL;
    *out_length = 0;
    char *json = json_dumps(message, JSON_COMPACT | JSON_SORT_KEYS);
    if (!json) return MAELYS_MCP_ERR_MEMORY;
    size_t json_length = strlen(json);
    if (core->framing == MAELYS_MCP_JSONRPC_JSON_LINES) {
        char *frame = malloc(json_length + 1u);
        if (!frame) { free(json); return MAELYS_MCP_ERR_MEMORY; }
        memcpy(frame, json, json_length);
        frame[json_length] = '\n';
        *out_bytes = frame;
        *out_length = json_length + 1u;
    } else {
        char header[128];
        int count = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", json_length);
        if (count <= 0 || (size_t)count >= sizeof(header)) { free(json); return MAELYS_MCP_ERR_PROTOCOL; }
        size_t header_length = (size_t)count;
        char *frame = malloc(header_length + json_length);
        if (!frame) { free(json); return MAELYS_MCP_ERR_MEMORY; }
        memcpy(frame, header, header_length);
        memcpy(frame + header_length, json, json_length);
        *out_bytes = frame;
        *out_length = header_length + json_length;
    }
    free(json);
    return MAELYS_MCP_OK;
}

int maelys_mcp_jsonrpc_core_has_buffered_bytes(const maelys_mcp_jsonrpc_core_t *core) {
    return core && core->length > 0;
}
