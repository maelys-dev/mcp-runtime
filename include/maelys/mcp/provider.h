#pragma once

#include <stddef.h>
#include <jansson.h>

#include "maelys/mcp/error.h"
#include "maelys/mcp/resources.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_mcp_provider maelys_mcp_provider_t;

#define MAELYS_MCP_PROVIDER_PROTOCOL "maelys-provider/3"
#define MAELYS_MCP_DEFAULT_PROVIDER_DESCRIBE_TIMEOUT_MS 5000u
#define MAELYS_MCP_DEFAULT_PROVIDER_CALL_TIMEOUT_MS 300000u
#define MAELYS_MCP_DEFAULT_PROVIDER_SHUTDOWN_TIMEOUT_MS 2000u

typedef enum maelys_mcp_tool_effect {
    MAELYS_MCP_EFFECT_UNSPECIFIED = 0,
    MAELYS_MCP_EFFECT_READ = 1,
    MAELYS_MCP_EFFECT_PREVIEW = 2,
    MAELYS_MCP_EFFECT_APPLY = 3,
    MAELYS_MCP_EFFECT_COMMIT = 4,
    MAELYS_MCP_EFFECT_EXECUTE = 5
} maelys_mcp_tool_effect_t;

typedef enum maelys_mcp_provider_event_kind {
    MAELYS_MCP_PROVIDER_EVENT_RESOURCE_UPDATED = 1,
    MAELYS_MCP_PROVIDER_EVENT_RESOURCES_LIST_CHANGED = 2,
    MAELYS_MCP_PROVIDER_EVENT_TOOLS_LIST_CHANGED = 3
} maelys_mcp_provider_event_kind_t;

typedef struct maelys_mcp_provider_event {
    maelys_mcp_provider_event_kind_t kind;
    const char *resource_uri;
} maelys_mcp_provider_event_t;

/* Thread-safe after the provider has been registered with a runtime. */
maelys_mcp_result_t maelys_mcp_provider_emit_event(
    maelys_mcp_provider_t *provider,
    const maelys_mcp_provider_event_t *event);

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

typedef enum maelys_mcp_provider_result_type {
    MAELYS_MCP_PROVIDER_RESULT_COMPLETE = 0,
    MAELYS_MCP_PROVIDER_RESULT_INPUT_REQUIRED = 1
} maelys_mcp_provider_result_type_t;

typedef struct maelys_mcp_provider_request {
    const char *tool_name;
    json_t *arguments;
    json_t *input_responses;
    json_t *request_state;
    json_t *client_capabilities;
} maelys_mcp_provider_request_t;

typedef struct maelys_mcp_provider_result {
    maelys_mcp_provider_result_type_t type;
    json_t *content;
    json_t *structured_content;
    json_t *input_requests;
    json_t *request_state;
    int is_error;
} maelys_mcp_provider_result_t;

void maelys_mcp_provider_result_init(maelys_mcp_provider_result_t *result);
void maelys_mcp_provider_result_clear(maelys_mcp_provider_result_t *result);

typedef maelys_mcp_result_t (*maelys_mcp_provider_call_fn)(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error);

typedef maelys_mcp_result_t (*maelys_mcp_provider_read_resource_fn)(
    void *context,
    const maelys_mcp_resource_request_t *request,
    maelys_mcp_resource_result_t *out_result,
    char **out_error);

typedef void (*maelys_mcp_provider_destroy_fn)(void *context);

typedef struct maelys_mcp_provider_config {
    const char *name;
    const char *version;
    const maelys_mcp_tool_t *tools;
    size_t tool_count;
    const maelys_mcp_resource_t *resources;
    size_t resource_count;
    const maelys_mcp_resource_template_t *resource_templates;
    size_t resource_template_count;
    maelys_mcp_provider_call_fn call;
    maelys_mcp_provider_read_resource_fn read_resource;
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

typedef struct maelys_mcp_provider_process_options {
    const char *executable_path;
    size_t max_message_bytes;
    unsigned int describe_timeout_ms;
    unsigned int call_timeout_ms;
    unsigned int shutdown_timeout_ms;
} maelys_mcp_provider_process_options_t;

maelys_mcp_result_t maelys_mcp_provider_spawn_with_options(
    const maelys_mcp_provider_process_options_t *options,
    maelys_mcp_provider_t **out_provider,
    char **out_error);

void maelys_mcp_provider_destroy(maelys_mcp_provider_t *provider);

#ifdef __cplusplus
}
#endif
