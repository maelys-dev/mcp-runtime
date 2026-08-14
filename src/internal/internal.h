#pragma once

#include <stddef.h>
#include <sys/types.h>
#include <jansson.h>

#include "maelys/mcp/runtime.h"

typedef struct maelys_mcp_owned_tool {
    char *name;
    char *title;
    char *description;
    json_t *input_schema;
    json_t *output_schema;
    maelys_mcp_tool_effect_t effect;
    struct maelys_mcp_provider *provider;
} maelys_mcp_owned_tool_t;

struct maelys_mcp_provider {
    char *name;
    char *version;
    maelys_mcp_owned_tool_t *tools;
    size_t tool_count;
    maelys_mcp_provider_call_fn call;
    maelys_mcp_provider_destroy_fn destroy;
    void *context;
};

struct maelys_mcp_runtime {
    char *server_name;
    char *server_version;
    char *instructions;
    size_t max_providers;
    size_t max_message_bytes;
    maelys_mcp_provider_t **providers;
    size_t provider_count;
    int legacy_initialize_received;
    int legacy_initialized;
    char legacy_client_name[128];
    maelys_mcp_authorize_fn authorize;
    maelys_mcp_audit_fn audit;
    void *policy_context;
};

typedef struct maelys_mcp_process_context {
    pid_t pid;
    int fd;
    size_t max_message_bytes;
    unsigned long long next_id;
} maelys_mcp_process_context_t;

char *maelys_mcp_strdup(const char *value);
maelys_mcp_result_t maelys_mcp_write_json_line(int fd, json_t *value);
maelys_mcp_result_t maelys_mcp_read_json_line(
    int fd,
    size_t max_bytes,
    json_t **out_value,
    char **out_error);
json_t *maelys_mcp_error_response(json_t *id, int code, const char *message, json_t *data);
json_t *maelys_mcp_success_response(json_t *id, json_t *result);
maelys_mcp_result_t maelys_mcp_validate_schema(json_t *schema, json_t *value, char **out_error);
maelys_mcp_result_t maelys_mcp_validate_schema_definition(
    json_t *schema,
    int require_object_root,
    char **out_error);
maelys_mcp_result_t maelys_mcp_isolate_stdout(int *out_transport_fd);
