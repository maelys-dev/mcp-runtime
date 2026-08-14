#pragma once

#include <stddef.h>
#include <jansson.h>

#include "maelys/mcp/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_mcp_provider maelys_mcp_provider_t;

typedef enum maelys_mcp_tool_effect {
    MAELYS_MCP_EFFECT_UNSPECIFIED = 0,
    MAELYS_MCP_EFFECT_READ = 1,
    MAELYS_MCP_EFFECT_PREVIEW = 2,
    MAELYS_MCP_EFFECT_APPLY = 3,
    MAELYS_MCP_EFFECT_COMMIT = 4,
    MAELYS_MCP_EFFECT_EXECUTE = 5
} maelys_mcp_tool_effect_t;

const char *maelys_mcp_tool_effect_string(maelys_mcp_tool_effect_t effect);
maelys_mcp_result_t maelys_mcp_tool_effect_parse(
    const char *value,
    maelys_mcp_tool_effect_t *out_effect);

typedef struct maelys_mcp_tool {
    const char *name;
    const char *title;
    const char *description;
    json_t *input_schema;
    json_t *output_schema;
    maelys_mcp_tool_effect_t effect;
} maelys_mcp_tool_t;

typedef maelys_mcp_result_t (*maelys_mcp_provider_call_fn)(
    void *context,
    const char *tool_name,
    json_t *arguments,
    json_t **out_result,
    char **out_error);

typedef void (*maelys_mcp_provider_destroy_fn)(void *context);

typedef struct maelys_mcp_provider_config {
    const char *name;
    const char *version;
    const maelys_mcp_tool_t *tools;
    size_t tool_count;
    maelys_mcp_provider_call_fn call;
    maelys_mcp_provider_destroy_fn destroy;
    void *context;
} maelys_mcp_provider_config_t;

maelys_mcp_result_t maelys_mcp_provider_create(
    const maelys_mcp_provider_config_t *config,
    maelys_mcp_provider_t **out_provider);

maelys_mcp_result_t maelys_mcp_provider_spawn(
    const char *executable_path,
    size_t max_message_bytes,
    maelys_mcp_provider_t **out_provider,
    char **out_error);

void maelys_mcp_provider_destroy(maelys_mcp_provider_t *provider);

#ifdef __cplusplus
}
#endif
