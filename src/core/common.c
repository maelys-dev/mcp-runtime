#include "src/internal/internal.h"
#include "src/jsonrpc/core.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAELYS_MCP_READ_CHUNK_BYTES 4096u

const char *maelys_mcp_result_string(maelys_mcp_result_t result) {
    switch (result) {
        case MAELYS_MCP_OK: return "ok";
        case MAELYS_MCP_ERR_ARGUMENT: return "invalid argument";
        case MAELYS_MCP_ERR_MEMORY: return "out of memory";
        case MAELYS_MCP_ERR_IO: return "I/O error";
        case MAELYS_MCP_ERR_PROTOCOL: return "protocol error";
        case MAELYS_MCP_ERR_PROVIDER: return "provider error";
        case MAELYS_MCP_ERR_NOT_FOUND: return "not found";
        case MAELYS_MCP_ERR_DENIED: return "denied";
        default: return "unknown error";
    }
}

char *maelys_mcp_strdup(const char *value) {
    if (!value) value = "";
    size_t size = strlen(value) + 1u;
    char *copy = malloc(size);
    if (copy) memcpy(copy, value, size);
    return copy;
}

int maelys_mcp_json_string_has_nul(const json_t *value) {
    if (!json_is_string(value)) return 0;
    const char *bytes = json_string_value(value);
    size_t length = json_string_length(value);
    return bytes && memchr(bytes, '\0', length) != NULL;
}

int maelys_mcp_json_string_equals(const json_t *value, const char *expected) {
    if (!json_is_string(value) || !expected) return 0;
    size_t expected_length = strlen(expected);
    return json_string_length(value) == expected_length &&
        memcmp(json_string_value(value), expected, expected_length) == 0;
}

int maelys_mcp_json_string_has_prefix(const json_t *value, const char *prefix) {
    if (!json_is_string(value) || !prefix) return 0;
    size_t prefix_length = strlen(prefix);
    return json_string_length(value) >= prefix_length &&
        memcmp(json_string_value(value), prefix, prefix_length) == 0;
}

static maelys_mcp_result_t write_all(int fd, const char *bytes, size_t length) {
    while (length > 0) {
        ssize_t written;
#ifdef MSG_NOSIGNAL
        written = send(fd, bytes, length, MSG_NOSIGNAL);
        if (written < 0 && errno == ENOTSOCK) written = write(fd, bytes, length);
#else
        written = write(fd, bytes, length);
#endif
        if (written < 0) {
            if (errno == EINTR) continue;
            return MAELYS_MCP_ERR_IO;
        }
        if (written == 0) return MAELYS_MCP_ERR_IO;
        bytes += (size_t)written;
        length -= (size_t)written;
    }
    return MAELYS_MCP_OK;
}

maelys_mcp_result_t maelys_mcp_write_json_line(int fd, json_t *value) {
    if (fd < 0 || !value) return MAELYS_MCP_ERR_ARGUMENT;
    maelys_mcp_jsonrpc_core_t core;
    memset(&core, 0, sizeof(core));
    core.framing = MAELYS_MCP_JSONRPC_JSON_LINES;
    char *encoded = NULL;
    size_t encoded_length = 0;
    maelys_mcp_result_t result = maelys_mcp_jsonrpc_core_serialize(
        &core, value, &encoded, &encoded_length);
    if (result == MAELYS_MCP_OK) result = write_all(fd, encoded, encoded_length);
    free(encoded);
    return result;
}

static void set_error(char **out_error, const char *message) {
    if (!out_error) return;
    free(*out_error);
    *out_error = maelys_mcp_strdup(message);
}

maelys_mcp_result_t maelys_mcp_line_reader_init(
    maelys_mcp_line_reader_t *reader,
    size_t max_bytes) {
    if (!reader || !max_bytes || max_bytes == (size_t)-1) return MAELYS_MCP_ERR_ARGUMENT;
    memset(reader, 0, sizeof(*reader));
    reader->max_bytes = max_bytes;
    return MAELYS_MCP_OK;
}

void maelys_mcp_line_reader_clear(maelys_mcp_line_reader_t *reader) {
    if (!reader) return;
    free(reader->buffer);
    memset(reader, 0, sizeof(*reader));
}

static maelys_mcp_result_t line_reader_reserve(
    maelys_mcp_line_reader_t *reader,
    size_t required) {
    if (required > reader->max_bytes + 1u) return MAELYS_MCP_ERR_PROTOCOL;
    if (required <= reader->capacity) return MAELYS_MCP_OK;
    size_t capacity = reader->capacity ? reader->capacity : MAELYS_MCP_READ_CHUNK_BYTES;
    if (capacity > reader->max_bytes + 1u) capacity = reader->max_bytes + 1u;
    while (capacity < required) {
        size_t next = capacity > (reader->max_bytes + 1u) / 2u ?
            reader->max_bytes + 1u : capacity * 2u;
        if (next <= capacity) return MAELYS_MCP_ERR_PROTOCOL;
        capacity = next;
    }
    char *grown = realloc(reader->buffer, capacity);
    if (!grown) return MAELYS_MCP_ERR_MEMORY;
    reader->buffer = grown;
    reader->capacity = capacity;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t line_reader_extract(
    maelys_mcp_line_reader_t *reader,
    json_t **out_value,
    char **out_error) {
    const char *newline = reader->buffer ? memchr(reader->buffer, '\n', reader->length) : NULL;
    if (!newline) return MAELYS_MCP_ERR_NOT_FOUND;
    size_t raw_length = (size_t)(newline - reader->buffer);
    size_t consumed = raw_length + 1u;
    size_t length = raw_length;
    if (length && reader->buffer[length - 1u] == '\r') --length;
    if (!length || length > reader->max_bytes) {
        memmove(reader->buffer, reader->buffer + consumed, reader->length - consumed);
        reader->length -= consumed;
        set_error(out_error, !length ? "empty JSON line" : "JSON line exceeds configured limit");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    json_error_t error;
    json_t *value = json_loadb(reader->buffer, length, JSON_REJECT_DUPLICATES, &error);
    memmove(reader->buffer, reader->buffer + consumed, reader->length - consumed);
    reader->length -= consumed;
    if (!value) {
        set_error(out_error, error.text);
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    *out_value = value;
    return MAELYS_MCP_OK;
}

maelys_mcp_result_t maelys_mcp_line_reader_read(
    maelys_mcp_line_reader_t *reader,
    int fd,
    json_t **out_value,
    char **out_error) {
    if (!reader || fd < 0 || !out_value || !reader->max_bytes) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    *out_value = NULL;
    for (;;) {
        maelys_mcp_result_t extracted = line_reader_extract(reader, out_value, out_error);
        if (extracted != MAELYS_MCP_ERR_NOT_FOUND) return extracted;
        if (reader->length > reader->max_bytes) {
            set_error(out_error, "JSON line exceeds configured limit");
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        size_t target = reader->length + MAELYS_MCP_READ_CHUNK_BYTES;
        if (target > reader->max_bytes + 1u) target = reader->max_bytes + 1u;
        maelys_mcp_result_t reserved = line_reader_reserve(reader, target);
        if (reserved != MAELYS_MCP_OK) return reserved;
        size_t available = reader->capacity - reader->length;
        if (!available) {
            set_error(out_error, "JSON line exceeds configured limit");
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        ssize_t count = read(fd, reader->buffer + reader->length, available);
        if (count < 0) {
            if (errno == EINTR) continue;
            set_error(out_error, "read failed");
            return MAELYS_MCP_ERR_IO;
        }
        if (count == 0) {
            if (!reader->length) return MAELYS_MCP_ERR_NOT_FOUND;
            set_error(out_error, "unexpected EOF in JSON line");
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        reader->length += (size_t)count;
    }
}

maelys_mcp_result_t maelys_mcp_read_json_line(
    int fd,
    size_t max_bytes,
    json_t **out_value,
    char **out_error) {
    maelys_mcp_line_reader_t reader = {0};
    maelys_mcp_result_t status = maelys_mcp_line_reader_init(&reader, max_bytes);
    if (status == MAELYS_MCP_OK) {
        status = maelys_mcp_line_reader_read(&reader, fd, out_value, out_error);
    }
    maelys_mcp_line_reader_clear(&reader);
    return status;
}

json_t *maelys_mcp_error_response(json_t *id, int code, const char *message, json_t *data) {
    json_t *root = json_object();
    json_t *error = json_object();
    if (!root || !error) goto failed;
    if (json_object_set_new(root, "jsonrpc", json_string("2.0")) != 0 ||
        json_object_set(root, "id", id ? id : json_null()) != 0 ||
        json_object_set_new(error, "code", json_integer(code)) != 0 ||
        json_object_set_new(error, "message", json_string(message ? message : "Error")) != 0 ||
        (data && json_object_set(error, "data", data) != 0) ||
        json_object_set(root, "error", error) != 0) goto failed;
    json_decref(error);
    return root;
failed:
    if (root) json_decref(root);
    if (error) json_decref(error);
    return NULL;
}

json_t *maelys_mcp_success_response(json_t *id, json_t *result) {
    json_t *root = json_object();
    if (!result) result = json_object();
    if (!root || !result) {
        if (root) json_decref(root);
        if (result) json_decref(result);
        return NULL;
    }
    if (json_object_set_new(root, "jsonrpc", json_string("2.0")) != 0 ||
        json_object_set(root, "id", id ? id : json_null()) != 0 ||
        json_object_set(root, "result", result) != 0) {
        json_decref(root);
        json_decref(result);
        return NULL;
    }
    json_decref(result);
    return root;
}
