#include "maelys/mcp/content.h"
#include "src/internal/internal.h"

#include <stdlib.h>
#include <string.h>

static maelys_mcp_result_t invalid(char **out_error, const char *message) {
    if (out_error) {
        free(*out_error);
        *out_error = maelys_mcp_strdup(message);
    }
    return MAELYS_MCP_ERR_PROTOCOL;
}

static int valid_base64(const json_t *value, size_t max_encoded_bytes) {
    if (!json_is_string(value) || maelys_mcp_json_string_has_nul(value)) return 0;
    const char *bytes = json_string_value(value);
    size_t length = json_string_length(value);
    if (!length || length > max_encoded_bytes || length % 4u != 0u) return 0;
    size_t padding = 0;
    if (length && bytes[length - 1u] == '=') padding++;
    if (length > 1u && bytes[length - 2u] == '=') padding++;
    for (size_t index = 0; index < length - padding; ++index) {
        unsigned char c = (unsigned char)bytes[index];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '+' || c == '/')) return 0;
    }
    for (size_t index = length - padding; index < length; ++index) {
        if (bytes[index] != '=') return 0;
    }
    return 1;
}

static int optional_object(json_t *block, const char *name) {
    json_t *value = json_object_get(block, name);
    return !value || json_is_object(value);
}

static maelys_mcp_result_t validate_block(
    json_t *block,
    size_t max_encoded_bytes,
    char **out_error) {
    if (!json_is_object(block)) return invalid(out_error, "content block must be an object");
    json_t *type = json_object_get(block, "type");
    if (!json_is_string(type) || !optional_object(block, "annotations") ||
        !optional_object(block, "_meta")) {
        return invalid(out_error, "content block has invalid common fields");
    }
    if (maelys_mcp_json_string_equals(type, "text")) {
        return json_is_string(json_object_get(block, "text")) ? MAELYS_MCP_OK :
            invalid(out_error, "text content requires text");
    }
    if (maelys_mcp_json_string_equals(type, "image") ||
        maelys_mcp_json_string_equals(type, "audio")) {
        json_t *data = json_object_get(block, "data");
        json_t *mime = json_object_get(block, "mimeType");
        const char *prefix = maelys_mcp_json_string_equals(type, "image") ? "image/" : "audio/";
        if (!json_is_string(data) || !json_is_string(mime) ||
            maelys_mcp_json_string_has_nul(mime) ||
            !maelys_mcp_json_string_has_prefix(mime, prefix) ||
            json_string_length(mime) == strlen(prefix) ||
            !valid_base64(data, max_encoded_bytes)) {
            return invalid(out_error, maelys_mcp_json_string_equals(type, "image") ?
                "image content requires base64 data and an image MIME type" :
                "audio content requires base64 data and an audio MIME type");
        }
        return MAELYS_MCP_OK;
    }
    if (maelys_mcp_json_string_equals(type, "resource_link")) {
        json_t *name = json_object_get(block, "name");
        json_t *uri = json_object_get(block, "uri");
        json_t *size = json_object_get(block, "size");
        return json_is_string(name) && json_string_length(name) > 0u &&
            json_is_string(uri) && json_string_length(uri) > 0u &&
            !maelys_mcp_json_string_has_nul(name) && !maelys_mcp_json_string_has_nul(uri) &&
            (!size || (json_is_integer(size) && json_integer_value(size) >= 0)) ? MAELYS_MCP_OK :
            invalid(out_error, "resource_link content requires name and uri");
    }
    if (maelys_mcp_json_string_equals(type, "resource")) {
        json_t *resource = json_object_get(block, "resource");
        json_t *uri = json_is_object(resource) ? json_object_get(resource, "uri") : NULL;
        json_t *text = json_is_object(resource) ? json_object_get(resource, "text") : NULL;
        json_t *blob = json_is_object(resource) ? json_object_get(resource, "blob") : NULL;
        json_t *mime = json_is_object(resource) ? json_object_get(resource, "mimeType") : NULL;
        if (!json_is_string(uri) || json_string_length(uri) == 0u ||
            maelys_mcp_json_string_has_nul(uri) ||
            (mime && (!json_is_string(mime) || maelys_mcp_json_string_has_nul(mime))) ||
            (!!text == !!blob) ||
            (text && !json_is_string(text)) ||
            (blob && !valid_base64(blob, max_encoded_bytes))) {
            return invalid(out_error,
                "embedded resource requires uri and exactly one of text or base64 blob");
        }
        return MAELYS_MCP_OK;
    }
    return invalid(out_error, "unsupported content block type");
}

maelys_mcp_result_t maelys_mcp_validate_content(
    json_t *content,
    size_t max_encoded_bytes,
    char **out_error) {
    if (!json_is_array(content) || json_array_size(content) == 0u) {
        return invalid(out_error, "content must be a non-empty array");
    }
    if (!max_encoded_bytes) max_encoded_bytes = MAELYS_MCP_DEFAULT_MAX_MESSAGE_BYTES;
    size_t encoded_size = json_dumpb(content, NULL, 0u, JSON_COMPACT);
    if (!encoded_size || encoded_size > max_encoded_bytes) {
        return invalid(out_error, "content exceeds the configured encoded size limit");
    }
    size_t index;
    json_t *block;
    json_array_foreach(content, index, block) {
        maelys_mcp_result_t status = validate_block(block, max_encoded_bytes, out_error);
        if (status != MAELYS_MCP_OK) return status;
    }
    return MAELYS_MCP_OK;
}
