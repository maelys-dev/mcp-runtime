#include "src/internal/internal.h"

#include <string.h>

extern const maelys_mcp_module_descriptor_t maelys_mcp_tools_module;
extern const maelys_mcp_module_descriptor_t maelys_mcp_mrtr_module;
extern const maelys_mcp_module_descriptor_t maelys_mcp_resources_module;
extern const maelys_mcp_module_descriptor_t maelys_mcp_subscriptions_module;

const maelys_mcp_module_descriptor_t *maelys_mcp_module_descriptor(
    maelys_mcp_module_kind_t kind) {
    switch (kind) {
        case MAELYS_MCP_MODULE_TOOLS: return &maelys_mcp_tools_module;
        case MAELYS_MCP_MODULE_MRTR: return &maelys_mcp_mrtr_module;
        case MAELYS_MCP_MODULE_RESOURCES: return &maelys_mcp_resources_module;
        case MAELYS_MCP_MODULE_SUBSCRIPTIONS: return &maelys_mcp_subscriptions_module;
    }
    return NULL;
}

int maelys_mcp_runtime_module_enabled(
    const maelys_mcp_runtime_t *runtime,
    maelys_mcp_module_kind_t module) {
    if (!runtime) return 0;
    for (size_t index = 0; index < runtime->module_count; ++index) {
        if (runtime->modules[index]->kind == module) return 1;
    }
    return 0;
}

maelys_mcp_result_t maelys_mcp_runtime_enable_module(
    maelys_mcp_runtime_t *runtime,
    maelys_mcp_module_kind_t module) {
    if (!runtime) return MAELYS_MCP_ERR_ARGUMENT;
    if (maelys_mcp_runtime_module_enabled(runtime, module)) return MAELYS_MCP_OK;
    const maelys_mcp_module_descriptor_t *descriptor =
        maelys_mcp_module_descriptor(module);
    if (!descriptor || runtime->module_count == MAELYS_MCP_MAX_MODULES) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    runtime->modules[runtime->module_count++] = descriptor;
    return MAELYS_MCP_OK;
}

json_t *maelys_mcp_runtime_capabilities(
    const maelys_mcp_runtime_t *runtime,
    int modern) {
    if (!runtime) return NULL;
    json_t *root = json_object();
    if (!root) return NULL;
    for (size_t index = 0; index < runtime->module_count; ++index) {
        const maelys_mcp_module_descriptor_t *module = runtime->modules[index];
        if (!module->capability_name) continue;
        json_t *capability = module->capability ?
            module->capability(runtime, modern) : json_object();
        if (!capability ||
            json_object_set(root, module->capability_name, capability) != 0) {
            if (capability) json_decref(capability);
            json_decref(root);
            return NULL;
        }
        json_decref(capability);
    }
    return root;
}
