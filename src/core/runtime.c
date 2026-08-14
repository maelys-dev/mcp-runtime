#include "src/internal/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char **out_error, const char *message) {
    if (!out_error) return;
    free(*out_error);
    *out_error = maelys_mcp_strdup(message);
}

static json_t *server_info(maelys_mcp_runtime_t *runtime) {
    json_t *info = json_object();
    if (!info ||
        json_object_set_new(info, "name", json_string(runtime->server_name)) != 0 ||
        json_object_set_new(info, "version", json_string(runtime->server_version)) != 0) {
        if (info) json_decref(info);
        return NULL;
    }
    return info;
}

int maelys_mcp_add_server_meta(
    maelys_mcp_runtime_t *runtime,
    json_t *result,
    const char *result_type) {
    json_t *meta = json_object();
    json_t *info = server_info(runtime);
    if (!meta || !info || !result_type) {
        if (meta) json_decref(meta);
        if (info) json_decref(info);
        return -1;
    }
    if (json_object_set_new(result, "resultType", json_string(result_type)) != 0 ||
        json_object_set(meta, "io.modelcontextprotocol/serverInfo", info) != 0 ||
        json_object_set(result, "_meta", meta) != 0) {
        json_decref(meta);
        json_decref(info);
        return -1;
    }
    json_decref(meta);
    json_decref(info);
    return 0;
}

maelys_mcp_result_t maelys_mcp_runtime_create(
    const maelys_mcp_runtime_config_t *config,
    maelys_mcp_runtime_t **out_runtime) {
    if (!config || !out_runtime || !config->server_name || !*config->server_name ||
        !config->server_version || !*config->server_version) return MAELYS_MCP_ERR_ARGUMENT;
    *out_runtime = NULL;
    maelys_mcp_runtime_t *runtime = calloc(1u, sizeof(*runtime));
    if (!runtime) return MAELYS_MCP_ERR_MEMORY;
    runtime->server_name = maelys_mcp_strdup(config->server_name);
    runtime->server_version = maelys_mcp_strdup(config->server_version);
    runtime->instructions = maelys_mcp_strdup(config->instructions);
    runtime->max_providers = config->max_providers ? config->max_providers : 16u;
    runtime->max_message_bytes = config->max_message_bytes ?
        config->max_message_bytes : MAELYS_MCP_DEFAULT_MAX_MESSAGE_BYTES;
    runtime->authorize = config->authorize;
    runtime->audit = config->audit;
    runtime->policy_context = config->policy_context;
    runtime->providers = calloc(runtime->max_providers, sizeof(*runtime->providers));
    if (!runtime->server_name || !runtime->server_version || !runtime->instructions ||
        !runtime->providers) {
        maelys_mcp_runtime_destroy(runtime);
        return MAELYS_MCP_ERR_MEMORY;
    }
    *out_runtime = runtime;
    return MAELYS_MCP_OK;
}

void maelys_mcp_runtime_destroy(maelys_mcp_runtime_t *runtime) {
    if (!runtime) return;
    for (size_t index = 0; index < runtime->provider_count; ++index) {
        maelys_mcp_provider_destroy(runtime->providers[index]);
    }
    free(runtime->providers);
    free(runtime->server_name);
    free(runtime->server_version);
    free(runtime->instructions);
    free(runtime);
}

maelys_mcp_result_t maelys_mcp_runtime_add_provider(
    maelys_mcp_runtime_t *runtime,
    maelys_mcp_provider_t *provider,
    char **out_error) {
    if (!runtime || !provider) return MAELYS_MCP_ERR_ARGUMENT;
    if (!maelys_mcp_runtime_module_enabled(runtime, MAELYS_MCP_MODULE_TOOLS)) {
        set_error(out_error, "tools module is not enabled");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    if (runtime->provider_count == runtime->max_providers) {
        set_error(out_error, "provider capacity reached");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    for (size_t candidate = 0; candidate < provider->tool_count; ++candidate) {
        for (size_t provider_index = 0; provider_index < runtime->provider_count; ++provider_index) {
            maelys_mcp_provider_t *existing = runtime->providers[provider_index];
            for (size_t tool_index = 0; tool_index < existing->tool_count; ++tool_index) {
                if (strcmp(provider->tools[candidate].name, existing->tools[tool_index].name) == 0) {
                    set_error(out_error, "duplicate tool name");
                    return MAELYS_MCP_ERR_ARGUMENT;
                }
            }
        }
    }
    runtime->providers[runtime->provider_count++] = provider;
    return MAELYS_MCP_OK;
}

static const char *client_name_from_params(json_t *params) {
    json_t *meta = json_is_object(params) ? json_object_get(params, "_meta") : NULL;
    json_t *client = json_is_object(meta) ?
        json_object_get(meta, "io.modelcontextprotocol/clientInfo") : NULL;
    json_t *name = json_is_object(client) ? json_object_get(client, "name") : NULL;
    return json_is_string(name) ? json_string_value(name) : "unknown";
}

static json_t *initialize(maelys_mcp_runtime_t *runtime, json_t *id, json_t *params) {
    if (runtime->legacy_initialize_received) {
        return maelys_mcp_error_response(id, JSONRPC_INVALID_REQUEST,
            "Initialize already received", NULL);
    }
    json_t *version = json_is_object(params) ? json_object_get(params, "protocolVersion") : NULL;
    if (!maelys_mcp_json_string_equals(version, MAELYS_MCP_PROTOCOL_LEGACY)) {
        return maelys_mcp_error_response(id, JSONRPC_INVALID_PARAMS,
            "Unsupported legacy protocol version", NULL);
    }
    json_t *client = json_object_get(params, "clientInfo");
    json_t *name = json_is_object(client) ? json_object_get(client, "name") : NULL;
    (void)snprintf(runtime->legacy_client_name, sizeof(runtime->legacy_client_name),
        "%s", json_is_string(name) && !maelys_mcp_json_string_has_nul(name) ?
        json_string_value(name) : "unknown");
    runtime->legacy_initialize_received = 1;
    json_t *result = json_object();
    json_t *info = server_info(runtime);
    json_t *caps = maelys_mcp_runtime_capabilities(runtime);
    if (!result || !info || !caps ||
        json_object_set_new(result, "protocolVersion",
            json_string(MAELYS_MCP_PROTOCOL_LEGACY)) != 0 ||
        json_object_set(result, "capabilities", caps) != 0 ||
        json_object_set(result, "serverInfo", info) != 0) {
        if (result) json_decref(result);
        if (info) json_decref(info);
        if (caps) json_decref(caps);
        return maelys_mcp_error_response(id, JSONRPC_INTERNAL_ERROR, "Internal error", NULL);
    }
    if (*runtime->instructions) {
        (void)json_object_set_new(result, "instructions", json_string(runtime->instructions));
    }
    json_decref(info);
    json_decref(caps);
    return maelys_mcp_success_response(id, result);
}

static json_t *discover(maelys_mcp_runtime_t *runtime, json_t *id) {
    json_t *result = json_object();
    json_t *versions = json_array();
    json_t *caps = maelys_mcp_runtime_capabilities(runtime);
    if (!result || !versions || !caps ||
        json_array_append_new(versions, json_string(MAELYS_MCP_PROTOCOL_MODERN)) != 0 ||
        json_array_append_new(versions, json_string(MAELYS_MCP_PROTOCOL_LEGACY)) != 0 ||
        json_object_set(result, "supportedVersions", versions) != 0 ||
        json_object_set(result, "capabilities", caps) != 0 ||
        json_object_set_new(result, "ttlMs", json_integer(0)) != 0 ||
        json_object_set_new(result, "cacheScope", json_string("private")) != 0 ||
        maelys_mcp_add_server_meta(runtime, result, "complete") != 0) {
        if (result) json_decref(result);
        if (versions) json_decref(versions);
        if (caps) json_decref(caps);
        return maelys_mcp_error_response(id, JSONRPC_INTERNAL_ERROR, "Internal error", NULL);
    }
    if (*runtime->instructions) {
        (void)json_object_set_new(result, "instructions", json_string(runtime->instructions));
    }
    json_decref(versions);
    json_decref(caps);
    return maelys_mcp_success_response(id, result);
}

static json_t *unsupported_version(json_t *id, const char *requested) {
    json_t *data = json_pack("{s:[s,s],s:s}", "supported",
        MAELYS_MCP_PROTOCOL_MODERN, MAELYS_MCP_PROTOCOL_LEGACY,
        "requested", requested ? requested : "");
    json_t *response = maelys_mcp_error_response(id, MCP_UNSUPPORTED_VERSION,
        "Unsupported protocol version", data);
    if (data) json_decref(data);
    return response;
}

static json_t *validate_modern_metadata(json_t *id, json_t *params, const char **out_version) {
    json_t *meta = json_is_object(params) ? json_object_get(params, "_meta") : NULL;
    json_t *version = json_is_object(meta) ?
        json_object_get(meta, "io.modelcontextprotocol/protocolVersion") : NULL;
    json_t *capabilities = json_is_object(meta) ?
        json_object_get(meta, "io.modelcontextprotocol/clientCapabilities") : NULL;
    json_t *client = json_is_object(meta) ?
        json_object_get(meta, "io.modelcontextprotocol/clientInfo") : NULL;
    if (!json_is_string(version) || maelys_mcp_json_string_has_nul(version) ||
        !json_is_object(capabilities)) {
        return maelys_mcp_error_response(id, JSONRPC_INVALID_PARAMS,
            "Modern requests require protocolVersion and clientCapabilities metadata", NULL);
    }
    if (client) {
        json_t *name = json_is_object(client) ? json_object_get(client, "name") : NULL;
        json_t *client_version = json_is_object(client) ? json_object_get(client, "version") : NULL;
        if (!json_is_string(name) || maelys_mcp_json_string_has_nul(name) ||
            !json_is_string(client_version) || maelys_mcp_json_string_has_nul(client_version)) {
            return maelys_mcp_error_response(id, JSONRPC_INVALID_PARAMS,
                "clientInfo must contain name and version", NULL);
        }
    }
    *out_version = json_string_value(version);
    return maelys_mcp_json_string_equals(version, MAELYS_MCP_PROTOCOL_MODERN) ?
        NULL : unsupported_version(id, *out_version);
}

json_t *maelys_mcp_runtime_handle(maelys_mcp_runtime_t *runtime, json_t *request) {
    if (!runtime || !json_is_object(request)) {
        return maelys_mcp_error_response(NULL, JSONRPC_INVALID_REQUEST,
            "Invalid Request", NULL);
    }
    json_t *id = json_object_get(request, "id");
    json_t *jsonrpc = json_object_get(request, "jsonrpc");
    json_t *method = json_object_get(request, "method");
    json_t *params = json_object_get(request, "params");
    int valid_id = !id || json_is_integer(id) ||
        (json_is_string(id) && !maelys_mcp_json_string_has_nul(id));
    if (!maelys_mcp_json_string_equals(jsonrpc, "2.0") ||
        !json_is_string(method) || maelys_mcp_json_string_has_nul(method) || !valid_id) {
        return maelys_mcp_error_response(valid_id ? id : NULL,
            JSONRPC_INVALID_REQUEST, "Invalid Request", NULL);
    }
    const char *method_name = json_string_value(method);
    if (strcmp(method_name, "initialize") == 0) return initialize(runtime, id, params);
    if (strcmp(method_name, "notifications/initialized") == 0) {
        if (runtime->legacy_initialize_received) runtime->legacy_initialized = 1;
        return NULL;
    }
    if (!id) return NULL;

    json_t *meta = json_is_object(params) ? json_object_get(params, "_meta") : NULL;
    const char *modern_version = NULL;
    if (meta || strcmp(method_name, "server/discover") == 0) {
        json_t *metadata_error = validate_modern_metadata(id, params, &modern_version);
        if (metadata_error) return metadata_error;
    }
    if (strcmp(method_name, "server/discover") == 0) return discover(runtime, id);

    int modern = modern_version != NULL;
    if (!modern && !runtime->legacy_initialized) {
        return maelys_mcp_error_response(id, MCP_SERVER_NOT_INITIALIZED,
            "Server not initialized", NULL);
    }
    maelys_mcp_module_request_t module_request = {
        .id = id,
        .params = params,
        .protocol_version = modern ? modern_version : MAELYS_MCP_PROTOCOL_LEGACY,
        .client_name = modern ? client_name_from_params(params) : runtime->legacy_client_name,
        .modern = modern
    };
    for (size_t index = 0; index < runtime->module_count; ++index) {
        const maelys_mcp_module_descriptor_t *module = runtime->modules[index];
        if (module->handles && module->handles(method_name)) {
            return module->handle(runtime, method_name, &module_request);
        }
    }
    return maelys_mcp_error_response(id, JSONRPC_METHOD_NOT_FOUND,
        "Method not found", NULL);
}
