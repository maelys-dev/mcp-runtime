#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum maelys_mcp_result {
    MAELYS_MCP_OK = 0,
    MAELYS_MCP_ERR_ARGUMENT = 1,
    MAELYS_MCP_ERR_MEMORY = 2,
    MAELYS_MCP_ERR_IO = 3,
    MAELYS_MCP_ERR_PROTOCOL = 4,
    MAELYS_MCP_ERR_PROVIDER = 5,
    MAELYS_MCP_ERR_NOT_FOUND = 6,
    MAELYS_MCP_ERR_DENIED = 7,
    MAELYS_MCP_ERR_STATE = 8,
    MAELYS_MCP_ERR_TIMEOUT = 9,
    MAELYS_MCP_ERR_CLOSED = 10
} maelys_mcp_result_t;

const char *maelys_mcp_result_string(maelys_mcp_result_t result);

#ifdef __cplusplus
}
#endif
