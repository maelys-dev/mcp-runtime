#include "maelys/mcp.h"

#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size == 0u || size > MAELYS_MCP_DEFAULT_MAX_MESSAGE_BYTES) return 0;
    json_error_t parse_error;
    json_t *value = json_loadb((const char *)data, size,
        JSON_REJECT_DUPLICATES | JSON_DECODE_ANY, &parse_error);
    if (!value) return 0;
    char *error = NULL;
    (void)maelys_mcp_validate_content(value, 65536u, &error);
    free(error);
    json_decref(value);
    return 0;
}
