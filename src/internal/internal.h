#pragma once

#include <stddef.h>
#include <sys/types.h>
#include <jansson.h>

#include "maelys/mcp/content.h"
#include "maelys/mcp/runtime.h"

#define MAELYS_MCP_MAX_MODULES 16u

#define JSONRPC_INVALID_REQUEST (-32600)
#define JSONRPC_METHOD_NOT_FOUND (-32601)
#define JSONRPC_INVALID_PARAMS (-32602)
#define JSONRPC_INTERNAL_ERROR (-32603)
#define MCP_SERVER_NOT_INITIALIZED (-32002)
#define MCP_POLICY_DENIED (-32003)
#define MCP_MISSING_REQUIRED_CLIENT_CAPABILITY (-32021)
#define MCP_UNSUPPORTED_VERSION (-32022)

typedef struct maelys_mcp_owned_tool {
    char *name;
    char *title;
    char *description;
    json_t *input_schema;
    json_t *output_schema;
    maelys_mcp_tool_effect_t effect;
    struct maelys_mcp_provider *provider;
} maelys_mcp_owned_tool_t;

typedef struct maelys_mcp_owned_resource {
    char *uri;
    char *name;
    char *title;
    char *description;
    char *mime_type;
    int has_size;
    long long size;
    struct maelys_mcp_provider *provider;
} maelys_mcp_owned_resource_t;

typedef struct maelys_mcp_owned_resource_template {
    char *uri_template;
    char *name;
    char *title;
    char *description;
    char *mime_type;
    struct maelys_mcp_provider *provider;
} maelys_mcp_owned_resource_template_t;

struct maelys_mcp_provider {
    char *name;
    char *version;
    maelys_mcp_owned_tool_t *tools;
    size_t tool_count;
    maelys_mcp_owned_resource_t *resources;
    size_t resource_count;
    maelys_mcp_owned_resource_template_t *resource_templates;
    size_t resource_template_count;
    maelys_mcp_provider_call_fn call;
    maelys_mcp_provider_read_resource_fn read_resource;
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
    const struct maelys_mcp_module_descriptor *modules[MAELYS_MCP_MAX_MODULES];
    size_t module_count;
    int legacy_initialize_received;
    int legacy_initialized;
    char legacy_client_name[128];
    maelys_mcp_authorize_fn authorize;
    maelys_mcp_audit_fn audit;
    void *policy_context;
};

typedef struct maelys_mcp_module_request {
    json_t *id;
    json_t *params;
    const char *protocol_version;
    const char *client_name;
    int modern;
} maelys_mcp_module_request_t;

typedef struct maelys_mcp_module_descriptor {
    maelys_mcp_module_kind_t kind;
    const char *name;
    const char *capability_name;
    json_t *(*capability)(const maelys_mcp_runtime_t *runtime);
    int (*handles)(const char *method);
    json_t *(*handle)(
        maelys_mcp_runtime_t *runtime,
        const char *method,
        const maelys_mcp_module_request_t *request);
} maelys_mcp_module_descriptor_t;

typedef struct maelys_mcp_process_context {
    pid_t pid;
    int fd;
    size_t max_message_bytes;
    unsigned long long next_id;
    unsigned int describe_timeout_ms;
    unsigned int call_timeout_ms;
    unsigned int shutdown_timeout_ms;
    struct maelys_mcp_line_reader *reader;
} maelys_mcp_process_context_t;

typedef struct maelys_mcp_line_reader {
    char *buffer;
    size_t length;
    size_t capacity;
    size_t max_bytes;
} maelys_mcp_line_reader_t;

char *maelys_mcp_strdup(const char *value);
int maelys_mcp_json_string_has_nul(const json_t *value);
int maelys_mcp_json_string_equals(const json_t *value, const char *expected);
int maelys_mcp_json_string_has_prefix(const json_t *value, const char *prefix);
maelys_mcp_result_t maelys_mcp_write_json_line(int fd, json_t *value);
maelys_mcp_result_t maelys_mcp_line_reader_init(
    maelys_mcp_line_reader_t *reader,
    size_t max_bytes);
void maelys_mcp_line_reader_clear(maelys_mcp_line_reader_t *reader);
maelys_mcp_result_t maelys_mcp_line_reader_read(
    maelys_mcp_line_reader_t *reader,
    int fd,
    json_t **out_value,
    char **out_error);
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
const maelys_mcp_module_descriptor_t *maelys_mcp_module_descriptor(
    maelys_mcp_module_kind_t kind);
json_t *maelys_mcp_runtime_capabilities(const maelys_mcp_runtime_t *runtime);
int maelys_mcp_add_server_meta(
    maelys_mcp_runtime_t *runtime,
    json_t *result,
    const char *result_type);
