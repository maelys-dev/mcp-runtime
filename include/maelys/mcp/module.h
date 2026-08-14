#pragma once

#include "maelys/mcp/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_mcp_runtime maelys_mcp_runtime_t;

typedef enum maelys_mcp_module_kind {
    MAELYS_MCP_MODULE_TOOLS = 1,
    MAELYS_MCP_MODULE_MRTR = 2
} maelys_mcp_module_kind_t;

maelys_mcp_result_t maelys_mcp_runtime_enable_module(
    maelys_mcp_runtime_t *runtime,
    maelys_mcp_module_kind_t module);

int maelys_mcp_runtime_module_enabled(
    const maelys_mcp_runtime_t *runtime,
    maelys_mcp_module_kind_t module);

#ifdef __cplusplus
}
#endif
