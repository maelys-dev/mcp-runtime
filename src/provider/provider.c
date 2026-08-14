#include "src/internal/internal.h"

#include <stdlib.h>
#include <string.h>

const char *maelys_mcp_tool_effect_string(maelys_mcp_tool_effect_t effect) {
    switch (effect) {
        case MAELYS_MCP_EFFECT_UNSPECIFIED: return NULL;
        case MAELYS_MCP_EFFECT_READ: return "read";
        case MAELYS_MCP_EFFECT_PREVIEW: return "preview";
        case MAELYS_MCP_EFFECT_APPLY: return "apply";
        case MAELYS_MCP_EFFECT_COMMIT: return "commit";
        case MAELYS_MCP_EFFECT_EXECUTE: return "execute";
    }
    return NULL;
}

maelys_mcp_result_t maelys_mcp_tool_effect_parse(
    const char *value,
    maelys_mcp_tool_effect_t *out_effect) {
    if (!value || !out_effect) return MAELYS_MCP_ERR_ARGUMENT;
    static const maelys_mcp_tool_effect_t effects[] = {
        MAELYS_MCP_EFFECT_READ,
        MAELYS_MCP_EFFECT_PREVIEW,
        MAELYS_MCP_EFFECT_APPLY,
        MAELYS_MCP_EFFECT_COMMIT,
        MAELYS_MCP_EFFECT_EXECUTE
    };
    for (size_t index = 0; index < sizeof(effects) / sizeof(effects[0]); ++index) {
        if (strcmp(value, maelys_mcp_tool_effect_string(effects[index])) == 0) {
            *out_effect = effects[index];
            return MAELYS_MCP_OK;
        }
    }
    return MAELYS_MCP_ERR_ARGUMENT;
}

static void clear_tools(maelys_mcp_owned_tool_t *tools, size_t count) {
    if (!tools) return;
    for (size_t index = 0; index < count; ++index) {
        free(tools[index].name);
        free(tools[index].title);
        free(tools[index].description);
        if (tools[index].input_schema) json_decref(tools[index].input_schema);
        if (tools[index].output_schema) json_decref(tools[index].output_schema);
    }
    free(tools);
}

void maelys_mcp_provider_result_init(maelys_mcp_provider_result_t *result) {
    if (result) memset(result, 0, sizeof(*result));
}

void maelys_mcp_provider_result_clear(maelys_mcp_provider_result_t *result) {
    if (!result) return;
    if (result->content) json_decref(result->content);
    if (result->structured_content) json_decref(result->structured_content);
    if (result->input_requests) json_decref(result->input_requests);
    if (result->request_state) json_decref(result->request_state);
    maelys_mcp_provider_result_init(result);
}

maelys_mcp_result_t maelys_mcp_provider_create(
    const maelys_mcp_provider_config_t *config,
    maelys_mcp_provider_t **out_provider) {
    if (!config || !out_provider || !config->name || !*config->name ||
        !config->version || !*config->version || !config->call ||
        (config->tool_count && !config->tools)) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    *out_provider = NULL;
    maelys_mcp_provider_t *provider = calloc(1u, sizeof(*provider));
    if (!provider) return MAELYS_MCP_ERR_MEMORY;
    provider->name = maelys_mcp_strdup(config->name);
    provider->version = maelys_mcp_strdup(config->version);
    provider->call = config->call;
    provider->destroy = config->destroy;
    provider->context = config->context;
    provider->tool_count = config->tool_count;
    if (!provider->name || !provider->version) goto memory_error;
    if (config->tool_count) {
        provider->tools = calloc(config->tool_count, sizeof(*provider->tools));
        if (!provider->tools) goto memory_error;
    }
    for (size_t index = 0; index < config->tool_count; ++index) {
        const maelys_mcp_tool_t *source = &config->tools[index];
        maelys_mcp_owned_tool_t *target = &provider->tools[index];
        if (!source->name || !*source->name || !source->description ||
            !json_is_object(source->input_schema) ||
            !maelys_mcp_tool_effect_string(source->effect)) goto argument_error;
        if (maelys_mcp_validate_schema_definition(source->input_schema, 1, NULL) != MAELYS_MCP_OK ||
            (source->output_schema &&
             maelys_mcp_validate_schema_definition(source->output_schema, 0, NULL) != MAELYS_MCP_OK)) {
            goto argument_error;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (strcmp(source->name, config->tools[previous].name) == 0) goto argument_error;
        }
        target->name = maelys_mcp_strdup(source->name);
        target->title = maelys_mcp_strdup(source->title ? source->title : source->name);
        target->description = maelys_mcp_strdup(source->description);
        target->input_schema = json_incref(source->input_schema);
        target->output_schema = source->output_schema ? json_incref(source->output_schema) : NULL;
        target->effect = source->effect;
        target->provider = provider;
        if (!target->name || !target->title || !target->description) goto memory_error;
    }
    *out_provider = provider;
    return MAELYS_MCP_OK;

argument_error:
    clear_tools(provider->tools, provider->tool_count);
    free(provider->name);
    free(provider->version);
    free(provider);
    return MAELYS_MCP_ERR_ARGUMENT;
memory_error:
    clear_tools(provider->tools, provider->tool_count);
    free(provider->name);
    free(provider->version);
    free(provider);
    return MAELYS_MCP_ERR_MEMORY;
}

void maelys_mcp_provider_destroy(maelys_mcp_provider_t *provider) {
    if (!provider) return;
    if (provider->destroy) provider->destroy(provider->context);
    clear_tools(provider->tools, provider->tool_count);
    free(provider->name);
    free(provider->version);
    free(provider);
}
