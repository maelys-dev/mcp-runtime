#include "src/internal/internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    json_error_t parse_error;
    json_t *root = json_loadb((const char *)data, size, JSON_REJECT_DUPLICATES, &parse_error);
    if (!json_is_array(root) || json_array_size(root) != 2u) {
        if (root) json_decref(root);
        return 0;
    }
    json_t *schema = json_array_get(root, 0);
    json_t *value = json_array_get(root, 1);
    char *error = NULL;
    if (maelys_mcp_validate_schema_definition(schema, 0, &error) == MAELYS_MCP_OK) {
        free(error);
        error = NULL;
        (void)maelys_mcp_validate_schema(schema, value, &error);
    }
    free(error);
    json_decref(root);
    return 0;
}
