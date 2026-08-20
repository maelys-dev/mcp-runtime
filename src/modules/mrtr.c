#include "src/internal/internal.h"

#include <string.h>

const maelys_mcp_module_descriptor_t maelys_mcp_mrtr_module = {
    .kind = MAELYS_MCP_MODULE_MRTR,
    .name = "mrtr"
};

/*
 * Shared by every input_required path (tools/call and resources/read): each
 * inputRequest must name one of the three MRTR methods, and each method maps
 * to a client capability that must have been declared - per-request _meta on
 * modern, initialize on legacy. Returns -1 for a malformed set (with
 * *out_error), 1 when at least one required capability is missing (collected
 * into *out_required_capabilities for the -32021 response), 0 when the set is
 * valid and fully declared.
 */
int maelys_mcp_validate_input_requests(
    json_t *requests,
    json_t *client_capabilities,
    json_t **out_required_capabilities,
    char **out_error) {
    if (!json_is_object(requests) || json_object_size(requests) == 0u) {
        if (out_error) *out_error = maelys_mcp_strdup("inputRequests must be a non-empty object");
        return -1;
    }
    const char *key;
    json_t *value;
    json_object_foreach(requests, key, value) {
        json_t *method = json_is_object(value) ? json_object_get(value, "method") : NULL;
        json_t *params = json_is_object(value) ? json_object_get(value, "params") : NULL;
        if (!*key || !json_is_string(method) || maelys_mcp_json_string_has_nul(method) ||
            !json_is_object(params)) {
            if (out_error) *out_error = maelys_mcp_strdup(
                "each inputRequest requires a key, method, and params object");
            return -1;
        }
        const char *name = json_string_value(method);
        if (strcmp(name, "elicitation/create") != 0 &&
            strcmp(name, "sampling/createMessage") != 0 &&
            strcmp(name, "roots/list") != 0) {
            if (out_error) *out_error = maelys_mcp_strdup("unsupported inputRequest method");
            return -1;
        }
        const char *capability = strcmp(name, "elicitation/create") == 0 ? "elicitation" :
            (strcmp(name, "sampling/createMessage") == 0 ? "sampling" : "roots");
        if (!json_is_object(client_capabilities) ||
            !json_is_object(json_object_get(client_capabilities, capability))) {
            if (out_required_capabilities) {
                if (!*out_required_capabilities) *out_required_capabilities = json_object();
                if (*out_required_capabilities) {
                    (void)json_object_set_new(*out_required_capabilities,
                        capability, json_object());
                }
            }
        }
    }
    return out_required_capabilities && *out_required_capabilities ? 1 : 0;
}
