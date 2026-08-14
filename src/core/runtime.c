#include "src/internal/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSONRPC_INVALID_REQUEST (-32600)
#define JSONRPC_METHOD_NOT_FOUND (-32601)
#define JSONRPC_INVALID_PARAMS (-32602)
#define JSONRPC_INTERNAL_ERROR (-32603)
#define MCP_SERVER_NOT_INITIALIZED (-32002)
#define MCP_POLICY_DENIED (-32003)
#define MCP_UNSUPPORTED_VERSION (-32022)

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

static json_t *capabilities(void) {
    json_t *root = json_object();
    json_t *tools = json_object();
    if (!root || !tools || json_object_set(root, "tools", tools) != 0) {
        if (root) json_decref(root);
        if (tools) json_decref(tools);
        return NULL;
    }
    json_decref(tools);
    return root;
}

static int add_server_meta(maelys_mcp_runtime_t *runtime, json_t *result) {
    json_t *meta = json_object();
    json_t *info = server_info(runtime);
    if (!meta || !info) {
        if (meta) json_decref(meta);
        if (info) json_decref(info);
        return -1;
    }
    if (json_object_set_new(result, "resultType", json_string("complete")) != 0 ||
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

static maelys_mcp_owned_tool_t *find_tool(maelys_mcp_runtime_t *runtime, const char *name) {
    for (size_t provider_index = 0; provider_index < runtime->provider_count; ++provider_index) {
        maelys_mcp_provider_t *provider = runtime->providers[provider_index];
        for (size_t tool_index = 0; tool_index < provider->tool_count; ++tool_index) {
            if (strcmp(provider->tools[tool_index].name, name) == 0) return &provider->tools[tool_index];
        }
    }
    return NULL;
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
    if (!runtime->server_name || !runtime->server_version || !runtime->instructions || !runtime->providers) {
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
    if (runtime->provider_count == runtime->max_providers) {
        set_error(out_error, "provider capacity reached");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    for (size_t index = 0; index < provider->tool_count; ++index) {
        if (find_tool(runtime, provider->tools[index].name)) {
            set_error(out_error, "duplicate tool name");
            return MAELYS_MCP_ERR_ARGUMENT;
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

static int policy_allows(
    maelys_mcp_runtime_t *runtime,
    const char *protocol_version,
    const char *client_name,
    const maelys_mcp_owned_tool_t *tool,
    maelys_mcp_operation_t operation) {
    if (!runtime->authorize) return 1;
    maelys_mcp_request_context_t request = {
        .protocol_version = protocol_version,
        .client_name = client_name,
        .tool_name = tool ? tool->name : NULL,
        .operation = operation,
        .effect = tool ? tool->effect : MAELYS_MCP_EFFECT_UNSPECIFIED
    };
    return runtime->authorize(runtime->policy_context, &request) != 0;
}

static void audit(
    maelys_mcp_runtime_t *runtime,
    const char *protocol_version,
    const char *client_name,
    const maelys_mcp_owned_tool_t *tool,
    maelys_mcp_operation_t operation,
    maelys_mcp_result_t outcome) {
    if (!runtime->audit) return;
    maelys_mcp_request_context_t request = {
        .protocol_version = protocol_version,
        .client_name = client_name,
        .tool_name = tool ? tool->name : NULL,
        .operation = operation,
        .effect = tool ? tool->effect : MAELYS_MCP_EFFECT_UNSPECIFIED
    };
    runtime->audit(runtime->policy_context, &request, outcome);
}

static json_t *initialize(maelys_mcp_runtime_t *runtime, json_t *id, json_t *params) {
    if (runtime->legacy_initialize_received) {
        return maelys_mcp_error_response(id, JSONRPC_INVALID_REQUEST, "Initialize already received", NULL);
    }
    json_t *version = json_is_object(params) ? json_object_get(params, "protocolVersion") : NULL;
    if (!json_is_string(version) || strcmp(json_string_value(version), MAELYS_MCP_PROTOCOL_LEGACY) != 0) {
        return maelys_mcp_error_response(id, JSONRPC_INVALID_PARAMS, "Unsupported legacy protocol version", NULL);
    }
    json_t *client = json_object_get(params, "clientInfo");
    json_t *client_name = json_is_object(client) ? json_object_get(client, "name") : NULL;
    if (json_is_string(client_name)) {
        (void)snprintf(runtime->legacy_client_name, sizeof(runtime->legacy_client_name), "%s", json_string_value(client_name));
    } else {
        (void)snprintf(runtime->legacy_client_name, sizeof(runtime->legacy_client_name), "%s", "unknown");
    }
    runtime->legacy_initialize_received = 1;
    json_t *result = json_object();
    json_t *info = server_info(runtime);
    json_t *caps = capabilities();
    if (!result || !info || !caps ||
        json_object_set_new(result, "protocolVersion", json_string(MAELYS_MCP_PROTOCOL_LEGACY)) != 0 ||
        json_object_set(result, "capabilities", caps) != 0 ||
        json_object_set(result, "serverInfo", info) != 0) {
        if (result) json_decref(result);
        if (info) json_decref(info);
        if (caps) json_decref(caps);
        return maelys_mcp_error_response(id, JSONRPC_INTERNAL_ERROR, "Internal error", NULL);
    }
    if (*runtime->instructions) (void)json_object_set_new(result, "instructions", json_string(runtime->instructions));
    json_decref(info);
    json_decref(caps);
    return maelys_mcp_success_response(id, result);
}

static json_t *discover(maelys_mcp_runtime_t *runtime, json_t *id) {
    json_t *result = json_object();
    json_t *versions = json_array();
    json_t *caps = capabilities();
    if (!result || !versions || !caps ||
        json_array_append_new(versions, json_string(MAELYS_MCP_PROTOCOL_MODERN)) != 0 ||
        json_array_append_new(versions, json_string(MAELYS_MCP_PROTOCOL_LEGACY)) != 0 ||
        json_object_set(result, "supportedVersions", versions) != 0 ||
        json_object_set(result, "capabilities", caps) != 0 ||
        json_object_set_new(result, "ttlMs", json_integer(0)) != 0 ||
        json_object_set_new(result, "cacheScope", json_string("private")) != 0 ||
        add_server_meta(runtime, result) != 0) {
        if (result) json_decref(result);
        if (versions) json_decref(versions);
        if (caps) json_decref(caps);
        return maelys_mcp_error_response(id, JSONRPC_INTERNAL_ERROR, "Internal error", NULL);
    }
    if (*runtime->instructions) (void)json_object_set_new(result, "instructions", json_string(runtime->instructions));
    json_decref(versions);
    json_decref(caps);
    return maelys_mcp_success_response(id, result);
}

static json_t *tool_as_json(maelys_mcp_owned_tool_t *tool) {
    json_t *root = json_object();
    json_t *annotations = json_object();
    int read_only = tool->effect == MAELYS_MCP_EFFECT_READ ||
        tool->effect == MAELYS_MCP_EFFECT_PREVIEW;
    if (!root || !annotations ||
        json_object_set_new(root, "name", json_string(tool->name)) != 0 ||
        json_object_set_new(root, "title", json_string(tool->title)) != 0 ||
        json_object_set_new(root, "description", json_string(tool->description)) != 0 ||
        json_object_set(root, "inputSchema", tool->input_schema) != 0 ||
        (tool->output_schema && json_object_set(root, "outputSchema", tool->output_schema) != 0) ||
        json_object_set_new(annotations, "readOnlyHint", json_boolean(read_only)) != 0 ||
        json_object_set_new(annotations, "destructiveHint", json_boolean(!read_only)) != 0 ||
        json_object_set_new(root, "_meta", json_pack("{s:s}",
            "org.maelys/toolEffect", maelys_mcp_tool_effect_string(tool->effect))) != 0 ||
        json_object_set(root, "annotations", annotations) != 0) {
        if (root) json_decref(root);
        if (annotations) json_decref(annotations);
        return NULL;
    }
    json_decref(annotations);
    return root;
}

static json_t *list_tools(
    maelys_mcp_runtime_t *runtime,
    json_t *id,
    const char *protocol_version,
    const char *client_name,
    int modern) {
    json_t *result = json_object();
    json_t *tools = json_array();
    if (!result || !tools) goto failed;
    for (size_t provider_index = 0; provider_index < runtime->provider_count; ++provider_index) {
        maelys_mcp_provider_t *provider = runtime->providers[provider_index];
        for (size_t tool_index = 0; tool_index < provider->tool_count; ++tool_index) {
            maelys_mcp_owned_tool_t *tool = &provider->tools[tool_index];
            if (!policy_allows(runtime, protocol_version, client_name, tool, MAELYS_MCP_OPERATION_LIST)) continue;
            json_t *value = tool_as_json(tool);
            if (!value || json_array_append(tools, value) != 0) {
                if (value) json_decref(value);
                goto failed;
            }
            json_decref(value);
        }
    }
    if (json_object_set(result, "tools", tools) != 0) goto failed;
    if (modern && (json_object_set_new(result, "ttlMs", json_integer(0)) != 0 ||
        json_object_set_new(result, "cacheScope", json_string("private")) != 0 ||
        add_server_meta(runtime, result) != 0)) goto failed;
    json_decref(tools);
    return maelys_mcp_success_response(id, result);
failed:
    if (result) json_decref(result);
    if (tools) json_decref(tools);
    return maelys_mcp_error_response(id, JSONRPC_INTERNAL_ERROR, "Internal error", NULL);
}

static json_t *tool_result(maelys_mcp_runtime_t *runtime, json_t *id, json_t *payload, const char *error, int modern) {
    json_t *result = json_object();
    json_t *content = json_array();
    json_t *block = json_object();
    char *text = payload ? json_dumps(payload, JSON_COMPACT | JSON_SORT_KEYS) : NULL;
    if (!text) text = maelys_mcp_strdup(error ? error : "provider error");
    if (!result || !content || !block || !text ||
        json_object_set_new(block, "type", json_string("text")) != 0 ||
        json_object_set_new(block, "text", json_string(text)) != 0 ||
        json_array_append(content, block) != 0 ||
        json_object_set(result, "content", content) != 0 ||
        (payload && json_object_set(result, "structuredContent", payload) != 0) ||
        (error && json_object_set_new(result, "isError", json_true()) != 0) ||
        (modern && add_server_meta(runtime, result) != 0)) {
        free(text);
        if (result) json_decref(result);
        if (content) json_decref(content);
        if (block) json_decref(block);
        return maelys_mcp_error_response(id, JSONRPC_INTERNAL_ERROR, "Internal error", NULL);
    }
    free(text);
    json_decref(content);
    json_decref(block);
    return maelys_mcp_success_response(id, result);
}

static json_t *call_tool(
    maelys_mcp_runtime_t *runtime,
    json_t *id,
    json_t *params,
    const char *protocol_version,
    const char *client_name,
    int modern) {
    json_t *name = json_is_object(params) ? json_object_get(params, "name") : NULL;
    json_t *arguments = json_is_object(params) ? json_object_get(params, "arguments") : NULL;
    if (!json_is_string(name) || (arguments && !json_is_object(arguments))) {
        return maelys_mcp_error_response(id, JSONRPC_INVALID_PARAMS, "Invalid tool call", NULL);
    }
    maelys_mcp_owned_tool_t *tool = find_tool(runtime, json_string_value(name));
    if (!tool) return maelys_mcp_error_response(id, JSONRPC_INVALID_PARAMS, "Unknown tool", NULL);
    json_t *owned_arguments = NULL;
    if (!arguments) {
        owned_arguments = json_object();
        arguments = owned_arguments;
    }
    char *validation_error = NULL;
    maelys_mcp_result_t status = maelys_mcp_validate_schema(tool->input_schema, arguments, &validation_error);
    if (status != MAELYS_MCP_OK) {
        json_t *data = json_pack("{s:s}", "detail", validation_error ? validation_error : "schema validation failed");
        free(validation_error);
        if (owned_arguments) json_decref(owned_arguments);
        json_t *response = maelys_mcp_error_response(id, JSONRPC_INVALID_PARAMS, "Invalid tool arguments", data);
        if (data) json_decref(data);
        return response;
    }
    if (!policy_allows(runtime, protocol_version, client_name, tool, MAELYS_MCP_OPERATION_CALL)) {
        audit(runtime, protocol_version, client_name, tool, MAELYS_MCP_OPERATION_CALL, MAELYS_MCP_ERR_DENIED);
        if (owned_arguments) json_decref(owned_arguments);
        return maelys_mcp_error_response(id, MCP_POLICY_DENIED, "Policy denied", NULL);
    }
    json_t *payload = NULL;
    char *provider_error = NULL;
    status = tool->provider->call(tool->provider->context, tool->name, arguments, &payload, &provider_error);
    if (owned_arguments) json_decref(owned_arguments);
    if (status == MAELYS_MCP_OK && !payload) {
        status = MAELYS_MCP_ERR_PROVIDER;
        free(provider_error);
        provider_error = maelys_mcp_strdup("provider returned no result");
    }
    if (status == MAELYS_MCP_OK && tool->output_schema && payload) {
        char *output_error = NULL;
        status = maelys_mcp_validate_schema(tool->output_schema, payload, &output_error);
        if (status != MAELYS_MCP_OK) {
            free(provider_error);
            provider_error = output_error;
            output_error = NULL;
        }
        free(output_error);
    }
    if (status != MAELYS_MCP_OK && payload) {
        json_decref(payload);
        payload = NULL;
    }
    audit(runtime, protocol_version, client_name, tool, MAELYS_MCP_OPERATION_CALL, status);
    json_t *response = tool_result(runtime, id, payload, status == MAELYS_MCP_OK ? NULL : provider_error, modern);
    if (payload) json_decref(payload);
    free(provider_error);
    return response;
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
    if (!json_is_string(version) || !json_is_object(capabilities)) {
        return maelys_mcp_error_response(id, JSONRPC_INVALID_PARAMS,
            "Modern requests require protocolVersion and clientCapabilities metadata", NULL);
    }
    if (client) {
        json_t *name = json_is_object(client) ? json_object_get(client, "name") : NULL;
        json_t *client_version = json_is_object(client) ? json_object_get(client, "version") : NULL;
        if (!json_is_string(name) || !json_is_string(client_version)) {
            return maelys_mcp_error_response(id, JSONRPC_INVALID_PARAMS,
                "clientInfo must contain name and version", NULL);
        }
    }
    *out_version = json_string_value(version);
    if (strcmp(*out_version, MAELYS_MCP_PROTOCOL_MODERN) != 0) {
        return unsupported_version(id, *out_version);
    }
    return NULL;
}

json_t *maelys_mcp_runtime_handle(maelys_mcp_runtime_t *runtime, json_t *request) {
    if (!runtime || !json_is_object(request)) return maelys_mcp_error_response(NULL, JSONRPC_INVALID_REQUEST, "Invalid Request", NULL);
    json_t *id = json_object_get(request, "id");
    json_t *jsonrpc = json_object_get(request, "jsonrpc");
    json_t *method = json_object_get(request, "method");
    json_t *params = json_object_get(request, "params");
    int valid_id = !id || json_is_integer(id) || json_is_string(id);
    if (!json_is_string(jsonrpc) || strcmp(json_string_value(jsonrpc), "2.0") != 0 ||
        !json_is_string(method) || !valid_id) {
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
        return maelys_mcp_error_response(id, MCP_SERVER_NOT_INITIALIZED, "Server not initialized", NULL);
    }
    const char *protocol_version = modern ? modern_version : MAELYS_MCP_PROTOCOL_LEGACY;
    const char *client_name = modern ? client_name_from_params(params) : runtime->legacy_client_name;
    if (strcmp(method_name, "tools/list") == 0) {
        return list_tools(runtime, id, protocol_version, client_name, modern);
    }
    if (strcmp(method_name, "tools/call") == 0) {
        return call_tool(runtime, id, params, protocol_version, client_name, modern);
    }
    return maelys_mcp_error_response(id, JSONRPC_METHOD_NOT_FOUND, "Method not found", NULL);
}
