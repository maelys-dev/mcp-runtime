#pragma once

#include <stddef.h>
#include <jansson.h>

#include "maelys/mcp/error.h"
#include "maelys/mcp/module.h"
#include "maelys/mcp/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAELYS_MCP_PROTOCOL_MODERN "2026-07-28"
#define MAELYS_MCP_PROTOCOL_LEGACY "2025-11-25"
#define MAELYS_MCP_DEFAULT_MAX_MESSAGE_BYTES (1024u * 1024u)

typedef struct maelys_mcp_runtime maelys_mcp_runtime_t;

typedef enum maelys_mcp_operation {
    MAELYS_MCP_OPERATION_LIST = 0,
    MAELYS_MCP_OPERATION_CALL = 1,
    MAELYS_MCP_OPERATION_RESOURCE_LIST = 2,
    MAELYS_MCP_OPERATION_RESOURCE_TEMPLATE_LIST = 3,
    MAELYS_MCP_OPERATION_RESOURCE_READ = 4
} maelys_mcp_operation_t;

typedef struct maelys_mcp_request_context {
    const char *protocol_version;
    const char *client_name;
    const char *tool_name;
    const char *resource_uri;
    maelys_mcp_operation_t operation;
    maelys_mcp_tool_effect_t effect;
} maelys_mcp_request_context_t;

typedef int (*maelys_mcp_authorize_fn)(
    void *context,
    const maelys_mcp_request_context_t *request);

typedef void (*maelys_mcp_audit_fn)(
    void *context,
    const maelys_mcp_request_context_t *request,
    maelys_mcp_result_t outcome);

typedef struct maelys_mcp_runtime_config {
    const char *server_name;
    const char *server_version;
    const char *instructions;
    size_t max_providers;
    size_t max_message_bytes;
    maelys_mcp_authorize_fn authorize;
    maelys_mcp_audit_fn audit;
    void *policy_context;
} maelys_mcp_runtime_config_t;

maelys_mcp_result_t maelys_mcp_runtime_create(
    const maelys_mcp_runtime_config_t *config,
    maelys_mcp_runtime_t **out_runtime);

void maelys_mcp_runtime_destroy(maelys_mcp_runtime_t *runtime);

maelys_mcp_result_t maelys_mcp_runtime_add_provider(
    maelys_mcp_runtime_t *runtime,
    maelys_mcp_provider_t *provider,
    char **out_error);

json_t *maelys_mcp_runtime_handle(
    maelys_mcp_runtime_t *runtime,
    json_t *request);

maelys_mcp_result_t maelys_mcp_runtime_serve_stdio(
    maelys_mcp_runtime_t *runtime,
    int read_fd,
    int write_fd);

#ifdef __cplusplus
}
#endif
