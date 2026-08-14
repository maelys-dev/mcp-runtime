#pragma once

#include <stddef.h>
#include <jansson.h>

#include "maelys/mcp/error.h"

typedef enum maelys_mcp_jsonrpc_framing {
    MAELYS_MCP_JSONRPC_CONTENT_LENGTH = 0,
    MAELYS_MCP_JSONRPC_JSON_LINES = 1
} maelys_mcp_jsonrpc_framing_t;

typedef maelys_mcp_result_t (*maelys_mcp_jsonrpc_message_fn)(
    void *context,
    json_t *message);

typedef struct maelys_mcp_jsonrpc_core {
    maelys_mcp_jsonrpc_framing_t framing;
    size_t max_header_bytes;
    size_t max_body_bytes;
    size_t max_line_bytes;
    maelys_mcp_jsonrpc_message_fn on_message;
    void *context;
    char *buffer;
    size_t length;
    size_t capacity;
} maelys_mcp_jsonrpc_core_t;

typedef struct maelys_mcp_jsonrpc_options {
    maelys_mcp_jsonrpc_framing_t framing;
    size_t max_header_bytes;
    size_t max_body_bytes;
    size_t max_line_bytes;
    maelys_mcp_jsonrpc_message_fn on_message;
    void *context;
} maelys_mcp_jsonrpc_options_t;

maelys_mcp_result_t maelys_mcp_jsonrpc_core_init(
    maelys_mcp_jsonrpc_core_t *core,
    const maelys_mcp_jsonrpc_options_t *options);
void maelys_mcp_jsonrpc_core_clear(maelys_mcp_jsonrpc_core_t *core);
maelys_mcp_result_t maelys_mcp_jsonrpc_core_feed(
    maelys_mcp_jsonrpc_core_t *core,
    const void *bytes,
    size_t length);
maelys_mcp_result_t maelys_mcp_jsonrpc_core_serialize(
    const maelys_mcp_jsonrpc_core_t *core,
    json_t *message,
    char **out_bytes,
    size_t *out_length);
int maelys_mcp_jsonrpc_core_has_buffered_bytes(const maelys_mcp_jsonrpc_core_t *core);
