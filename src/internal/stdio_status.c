#include "src/internal/internal.h"

maelys_mcp_result_t maelys_mcp_stdio_finish_status(
    maelys_mcp_result_t primary_status,
    maelys_mcp_result_t close_status,
    maelys_mcp_result_t writer_status,
    maelys_mcp_result_t destroy_status,
    maelys_mcp_result_t flags_status) {
    if (primary_status != MAELYS_MCP_OK) return primary_status;
    if (close_status != MAELYS_MCP_OK) return close_status;
    if (writer_status != MAELYS_MCP_OK) return writer_status;
    if (destroy_status != MAELYS_MCP_OK) return destroy_status;
    return flags_status;
}
