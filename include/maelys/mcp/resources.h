#pragma once

#include <stddef.h>
#include <jansson.h>

#include "maelys/mcp/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_mcp_resource {
    const char *uri;
    const char *name;
    const char *title;
    const char *description;
    const char *mime_type;
    int has_size;
    long long size;
} maelys_mcp_resource_t;

typedef struct maelys_mcp_resource_template {
    const char *uri_template;
    const char *name;
    const char *title;
    const char *description;
    const char *mime_type;
} maelys_mcp_resource_template_t;

typedef enum maelys_mcp_resource_result_type {
    MAELYS_MCP_RESOURCE_RESULT_COMPLETE = 0,
    MAELYS_MCP_RESOURCE_RESULT_INPUT_REQUIRED = 1
} maelys_mcp_resource_result_type_t;

typedef struct maelys_mcp_resource_request {
    const char *uri;
    json_t *input_responses;
    json_t *request_state;
    json_t *client_capabilities;
} maelys_mcp_resource_request_t;

typedef struct maelys_mcp_resource_result {
    maelys_mcp_resource_result_type_t type;
    json_t *contents;
    json_t *input_requests;
    json_t *request_state;
} maelys_mcp_resource_result_t;

void maelys_mcp_resource_result_init(maelys_mcp_resource_result_t *result);
void maelys_mcp_resource_result_clear(maelys_mcp_resource_result_t *result);

maelys_mcp_result_t maelys_mcp_validate_resource_contents(
    json_t *contents,
    size_t max_encoded_bytes,
    char **out_error);

#ifdef __cplusplus
}
#endif
