#pragma once

#include <stddef.h>

#include "maelys/mcp/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_mcp_provider_sdk maelys_mcp_provider_sdk_t;

typedef maelys_mcp_result_t (*maelys_mcp_provider_sdk_call_fn)(
    maelys_mcp_provider_sdk_t *sdk,
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error);

typedef maelys_mcp_result_t (*maelys_mcp_provider_sdk_read_resource_fn)(
    maelys_mcp_provider_sdk_t *sdk,
    void *context,
    const maelys_mcp_resource_request_t *request,
    maelys_mcp_resource_result_t *out_result,
    char **out_error);

typedef maelys_mcp_result_t (*maelys_mcp_provider_sdk_activate_fn)(
    maelys_mcp_provider_sdk_t *sdk,
    void *context,
    char **out_error);

/* Called once when serve() starts shutting down. A provider that created event
 * producer threads must stop and join them here before returning. */
typedef void (*maelys_mcp_provider_sdk_shutdown_fn)(void *context);

typedef void (*maelys_mcp_provider_sdk_destroy_fn)(void *context);

typedef struct maelys_mcp_provider_sdk_config {
    const char *name;
    const char *version;
    const maelys_mcp_tool_t *tools;
    size_t tool_count;
    const maelys_mcp_resource_t *resources;
    size_t resource_count;
    const maelys_mcp_resource_template_t *resource_templates;
    size_t resource_template_count;
    maelys_mcp_provider_sdk_call_fn call;
    maelys_mcp_provider_sdk_read_resource_fn read_resource;
    maelys_mcp_provider_sdk_activate_fn activate;
    maelys_mcp_provider_sdk_shutdown_fn shutdown;
    maelys_mcp_provider_sdk_destroy_fn destroy;
    void *context;
} maelys_mcp_provider_sdk_config_t;

typedef struct maelys_mcp_provider_sdk_options {
    size_t max_message_bytes;
    int input_fd;       /* <= 0 means STDIN_FILENO. */
    int output_fd;      /* <= 0 means STDOUT_FILENO, or the isolated stdout fd. */
    /* Non-zero leaves stdout untouched when output_fd uses STDOUT_FILENO.
     * Zero is the safe default: protocol writes use a duplicated stdout fd and
     * application stdout is redirected to stderr. */
    int disable_stdout_isolation;
} maelys_mcp_provider_sdk_options_t;

/* The config and every descriptor it references remain caller-owned and must
 * stay valid until serve() returns. A callback transfers ownership of every
 * JSON value it assigns to out_result; callback errors must be
 * malloc-compatible allocations.
 *
 * emit_event() is thread-safe after activation. It may be called from provider
 * worker threads only while serve() is running. The shutdown callback must stop
 * and join every such worker before returning; after shutdown begins, new event
 * emissions are denied. */
maelys_mcp_result_t maelys_mcp_provider_sdk_serve(
    const maelys_mcp_provider_sdk_config_t *config,
    const maelys_mcp_provider_sdk_options_t *options);

maelys_mcp_result_t maelys_mcp_provider_sdk_emit_event(
    maelys_mcp_provider_sdk_t *sdk,
    const maelys_mcp_provider_event_t *event);

#ifdef __cplusplus
}
#endif
