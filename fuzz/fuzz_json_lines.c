#include "src/jsonrpc/core.h"

#include <stddef.h>
#include <stdint.h>

static maelys_mcp_result_t discard(void *context, json_t *message) {
    (void)context;
    json_decref(message);
    return MAELYS_MCP_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    maelys_mcp_jsonrpc_core_t core;
    maelys_mcp_jsonrpc_options_t options = {
        .framing = MAELYS_MCP_JSONRPC_JSON_LINES,
        .max_line_bytes = 4096,
        .on_message = discard
    };
    if (maelys_mcp_jsonrpc_core_init(&core, &options) != MAELYS_MCP_OK) return 0;
    size_t offset = 0;
    while (offset < size) {
        size_t chunk = 1u + (size_t)(data[offset] % 31u);
        if (chunk > size - offset) chunk = size - offset;
        if (maelys_mcp_jsonrpc_core_feed(&core, data + offset, chunk) != MAELYS_MCP_OK) break;
        offset += chunk;
    }
    (void)maelys_mcp_jsonrpc_core_finish(&core);
    maelys_mcp_jsonrpc_core_clear(&core);
    return 0;
}
