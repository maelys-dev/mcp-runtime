#include "maelys/mcp/resources.h"
#include "maelys/mcp/runtime.h"
#include "maelys/mcp/uri.h"
#include "src/internal/internal.h"

#include <stdlib.h>
#include <string.h>

static maelys_mcp_result_t resource_error(char **out_error, const char *message) {
    if (out_error) {
        free(*out_error);
        *out_error = maelys_mcp_strdup(message);
    }
    return MAELYS_MCP_ERR_PROTOCOL;
}

static int valid_base64(json_t *value, size_t max_encoded_bytes) {
    if (!json_is_string(value) || maelys_mcp_json_string_has_nul(value)) return 0;
    const char *bytes = json_string_value(value);
    size_t length = json_string_length(value);
    if (!length || length > max_encoded_bytes || length % 4u) return 0;
    size_t padding = 0;
    if (bytes[length - 1u] == '=') ++padding;
    if (length > 1u && bytes[length - 2u] == '=') ++padding;
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

void maelys_mcp_resource_result_init(maelys_mcp_resource_result_t *result) {
    if (result) memset(result, 0, sizeof(*result));
}

void maelys_mcp_resource_result_clear(maelys_mcp_resource_result_t *result) {
    if (!result) return;
    if (result->contents) json_decref(result->contents);
    if (result->input_requests) json_decref(result->input_requests);
    if (result->request_state) json_decref(result->request_state);
    maelys_mcp_resource_result_init(result);
}

maelys_mcp_result_t maelys_mcp_validate_resource_contents(
    json_t *contents,
    size_t max_encoded_bytes,
    char **out_error) {
    if (!json_is_array(contents) || json_array_size(contents) == 0u) {
        return resource_error(out_error, "resource contents must be a non-empty array");
    }
    if (!max_encoded_bytes) max_encoded_bytes = MAELYS_MCP_DEFAULT_MAX_MESSAGE_BYTES;
    size_t encoded = json_dumpb(contents, NULL, 0u, JSON_COMPACT);
    if (!encoded || encoded > max_encoded_bytes) {
        return resource_error(out_error, "resource contents exceed configured limit");
    }
    size_t index;
    json_t *item;
    json_array_foreach(contents, index, item) {
        json_t *uri_value = json_is_object(item) ? json_object_get(item, "uri") : NULL;
        json_t *mime = json_is_object(item) ? json_object_get(item, "mimeType") : NULL;
        json_t *text = json_is_object(item) ? json_object_get(item, "text") : NULL;
        json_t *blob = json_is_object(item) ? json_object_get(item, "blob") : NULL;
        json_t *meta = json_is_object(item) ? json_object_get(item, "_meta") : NULL;
        if (!json_is_string(uri_value) || maelys_mcp_json_string_has_nul(uri_value) ||
            (mime && (!json_is_string(mime) || maelys_mcp_json_string_has_nul(mime))) ||
            (meta && !json_is_object(meta)) || (!!text == !!blob) ||
            (text && !json_is_string(text)) || (blob && !valid_base64(blob, max_encoded_bytes))) {
            return resource_error(out_error,
                "resource content requires URI and exactly one valid text or blob field");
        }
        maelys_uri_t *uri = NULL;
        maelys_uri_options_t options = {.max_bytes = 8192u, .require_scheme = 1};
        maelys_mcp_result_t status = maelys_uri_parse(json_string_value(uri_value),
            json_string_length(uri_value), &options, &uri, out_error);
        maelys_uri_destroy(uri);
        if (status != MAELYS_MCP_OK) return status;
    }
    return MAELYS_MCP_OK;
}
