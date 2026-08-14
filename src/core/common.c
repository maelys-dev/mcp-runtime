#include "src/internal/internal.h"
#include "src/jsonrpc/core.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

maelys_mcp_result_t maelys_mcp_read_json_line(
    int fd,
    size_t max_bytes,
    json_t **out_value,
    char **out_error) {
    if (fd < 0 || !out_value || max_bytes == 0) return MAELYS_MCP_ERR_ARGUMENT;
    *out_value = NULL;
    char *buffer = malloc(max_bytes + 1u);
    if (!buffer) return MAELYS_MCP_ERR_MEMORY;
    size_t length = 0;
    for (;;) {
        char byte = 0;
        ssize_t count = read(fd, &byte, 1u);
        if (count < 0) {
            if (errno == EINTR) continue;
            free(buffer);
            set_error(out_error, "read failed");
            return MAELYS_MCP_ERR_IO;
        }
        if (count == 0) {
            free(buffer);
            if (length == 0) return MAELYS_MCP_ERR_NOT_FOUND;
            set_error(out_error, "unexpected EOF in JSON line");
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        if (byte == '\n') break;
        if (byte == '\r') continue;
        if (length == max_bytes) {
            free(buffer);
            set_error(out_error, "JSON line exceeds configured limit");
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        buffer[length++] = byte;
    }
    buffer[length] = '\0';
    if (length == 0) {
        free(buffer);
        set_error(out_error, "empty JSON line");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    json_error_t error;
    json_t *value = json_loadb(buffer, length, JSON_REJECT_DUPLICATES, &error);
    free(buffer);
    if (!value) {
        set_error(out_error, error.text);
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    *out_value = value;
    return MAELYS_MCP_OK;
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
