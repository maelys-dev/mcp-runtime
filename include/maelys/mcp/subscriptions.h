#pragma once

#include "maelys/mcp/error.h"
#include "maelys/mcp/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Emit events only to subscriptions whose negotiated filter accepts them. */
maelys_mcp_result_t maelys_mcp_runtime_notify_resource_updated(
    maelys_mcp_runtime_t *runtime,
    const char *uri);

maelys_mcp_result_t maelys_mcp_runtime_notify_resources_list_changed(
    maelys_mcp_runtime_t *runtime);

maelys_mcp_result_t maelys_mcp_runtime_notify_tools_list_changed(
    maelys_mcp_runtime_t *runtime);

/* Gracefully completes every open listen request and clears the registry. */
maelys_mcp_result_t maelys_mcp_runtime_complete_subscriptions(
    maelys_mcp_runtime_t *runtime);

#ifdef __cplusplus
}
#endif
