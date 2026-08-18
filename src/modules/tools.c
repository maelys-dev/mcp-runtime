#include "src/internal/internal.h"

#include <stdlib.h>
#include <string.h>

static maelys_mcp_owned_tool_t *find_tool(
    maelys_mcp_runtime_t *runtime,
    const char *name) {
    for (size_t provider_index = 0; provider_index < runtime->provider_count; ++provider_index) {
        maelys_mcp_provider_t *provider = runtime->providers[provider_index];
        for (size_t tool_index = 0; tool_index < provider->tool_count; ++tool_index) {
            if (strcmp(provider->tools[tool_index].name, name) == 0) {
                return &provider->tools[tool_index];
            }
        }
    }
    return NULL;
}

static int policy_allows(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_module_request_t *request,
    const maelys_mcp_owned_tool_t *tool,
    maelys_mcp_operation_t operation) {
    if (!runtime->authorize) return 1;
    maelys_mcp_request_context_t context = {
        .protocol_version = request->protocol_version,
        .client_name = request->client_name,
        .tool_name = tool ? tool->name : NULL,
        .operation = operation,
        .effect = tool ? tool->effect : MAELYS_MCP_EFFECT_UNSPECIFIED
    };
    return runtime->authorize(runtime->policy_context, &context) != 0;
}

static void audit(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_module_request_t *request,
    const maelys_mcp_owned_tool_t *tool,
    maelys_mcp_result_t outcome) {
    if (!runtime->audit) return;
    maelys_mcp_request_context_t context = {
        .protocol_version = request->protocol_version,
        .client_name = request->client_name,
        .tool_name = tool ? tool->name : NULL,
        .operation = MAELYS_MCP_OPERATION_CALL,
        .effect = tool ? tool->effect : MAELYS_MCP_EFFECT_UNSPECIFIED
    };
    runtime->audit(runtime->policy_context, &context, outcome);
}

static json_t *tool_as_json(maelys_mcp_owned_tool_t *tool) {
    json_t *root = json_object();
    json_t *annotations = json_object();
    int read_only = tool->effect == MAELYS_MCP_EFFECT_READ ||
        tool->effect == MAELYS_MCP_EFFECT_PREVIEW;
    const char *effect = maelys_mcp_tool_effect_string(tool->effect);
    if (!root || !annotations || !effect ||
        json_object_set_new(root, "name", json_string(tool->name)) != 0 ||
        json_object_set_new(root, "title", json_string(tool->title)) != 0 ||
        json_object_set_new(root, "description", json_string(tool->description)) != 0 ||
        json_object_set(root, "inputSchema", tool->input_schema) != 0 ||
        (tool->output_schema &&
            json_object_set(root, "outputSchema", tool->output_schema) != 0) ||
        json_object_set_new(annotations, "readOnlyHint", json_boolean(read_only)) != 0 ||
        json_object_set_new(annotations, "destructiveHint", json_boolean(!read_only)) != 0 ||
        json_object_set_new(root, "_meta",
            json_pack("{s:s}", "org.maelys/toolEffect", effect)) != 0 ||
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
    const maelys_mcp_module_request_t *request) {
    json_t *result = json_object();
    json_t *tools = json_array();
    if (!result || !tools) goto failed;
    for (size_t provider_index = 0; provider_index < runtime->provider_count; ++provider_index) {
        maelys_mcp_provider_t *provider = runtime->providers[provider_index];
        for (size_t tool_index = 0; tool_index < provider->tool_count; ++tool_index) {
            maelys_mcp_owned_tool_t *tool = &provider->tools[tool_index];
            if (!policy_allows(runtime, request, tool, MAELYS_MCP_OPERATION_LIST)) continue;
            json_t *value = tool_as_json(tool);
            if (!value || json_array_append(tools, value) != 0) {
                if (value) json_decref(value);
                goto failed;
            }
            json_decref(value);
        }
    }
    if (json_object_set(result, "tools", tools) != 0) goto failed;
    if (request->modern &&
        (json_object_set_new(result, "ttlMs", json_integer(0)) != 0 ||
         json_object_set_new(result, "cacheScope", json_string("private")) != 0 ||
         maelys_mcp_add_server_meta(runtime, result, "complete") != 0)) goto failed;
    json_decref(tools);
    return maelys_mcp_success_response(request->id, result);
failed:
    if (result) json_decref(result);
    if (tools) json_decref(tools);
    return maelys_mcp_error_response(request->id, JSONRPC_INTERNAL_ERROR,
        "Internal error", NULL);
}

static json_t *text_content(const char *text) {
    json_t *content = json_array();
    json_t *block = json_pack("{s:s,s:s}", "type", "text", "text", text);
    if (!content || !block || json_array_append(content, block) != 0) {
        if (content) json_decref(content);
        if (block) json_decref(block);
        return NULL;
    }
    json_decref(block);
    return content;
}

static int validate_input_requests(
    json_t *requests,
    json_t *client_capabilities,
    json_t **out_required_capabilities,
    char **out_error) {
    if (!json_is_object(requests) || json_object_size(requests) == 0u) {
        if (out_error) *out_error = maelys_mcp_strdup("inputRequests must be a non-empty object");
        return -1;
    }
    const char *key;
    json_t *value;
    json_object_foreach(requests, key, value) {
        json_t *method = json_is_object(value) ? json_object_get(value, "method") : NULL;
        json_t *params = json_is_object(value) ? json_object_get(value, "params") : NULL;
        if (!*key || !json_is_string(method) || maelys_mcp_json_string_has_nul(method) ||
            !json_is_object(params)) {
            if (out_error) *out_error = maelys_mcp_strdup(
                "each inputRequest requires a key, method, and params object");
            return -1;
        }
        const char *name = json_string_value(method);
        if (strcmp(name, "elicitation/create") != 0 &&
            strcmp(name, "sampling/createMessage") != 0 &&
            strcmp(name, "roots/list") != 0) {
            if (out_error) *out_error = maelys_mcp_strdup("unsupported inputRequest method");
            return -1;
        }
        const char *capability = strcmp(name, "elicitation/create") == 0 ? "elicitation" :
            (strcmp(name, "sampling/createMessage") == 0 ? "sampling" : "roots");
        if (!json_is_object(client_capabilities) ||
            !json_is_object(json_object_get(client_capabilities, capability))) {
            if (out_required_capabilities) {
                if (!*out_required_capabilities) *out_required_capabilities = json_object();
                if (*out_required_capabilities) {
                    (void)json_object_set_new(*out_required_capabilities,
                        capability, json_object());
                }
            }
        }
    }
    return out_required_capabilities && *out_required_capabilities ? 1 : 0;
}

static json_t *provider_failure(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_module_request_t *request,
    const char *message) {
    json_t *result = json_object();
    json_t *content = text_content(message ? message : "provider error");
    if (!result || !content ||
        json_object_set(result, "content", content) != 0 ||
        json_object_set_new(result, "isError", json_true()) != 0 ||
        (request->modern &&
            maelys_mcp_add_server_meta(runtime, result, "complete") != 0)) {
        if (result) json_decref(result);
        if (content) json_decref(content);
        return maelys_mcp_error_response(request->id, JSONRPC_INTERNAL_ERROR,
            "Internal error", NULL);
    }
    json_decref(content);
    return maelys_mcp_success_response(request->id, result);
}

static maelys_mcp_result_t validate_provider_result(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_owned_tool_t *tool,
    const maelys_mcp_provider_result_t *result,
    json_t *client_capabilities,
    json_t **out_required_capabilities,
    char **out_error) {
    if (result->type == MAELYS_MCP_PROVIDER_RESULT_INPUT_REQUIRED) {
        if (!maelys_mcp_runtime_module_enabled(runtime, MAELYS_MCP_MODULE_MRTR)) {
            if (out_error) *out_error = maelys_mcp_strdup(
                "input_required requires the MRTR module");
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        if ((!result->input_requests || json_object_size(result->input_requests) == 0u) &&
            !json_is_string(result->request_state)) {
            if (out_error) *out_error = maelys_mcp_strdup(
                "input_required requires inputRequests or requestState");
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        if (result->input_requests) {
            int validation = validate_input_requests(result->input_requests,
                client_capabilities, out_required_capabilities, out_error);
            if (validation < 0) return MAELYS_MCP_ERR_PROTOCOL;
            if (validation > 0) return MAELYS_MCP_ERR_DENIED;
        }
        if (result->request_state && (!json_is_string(result->request_state) ||
            json_string_length(result->request_state) == 0u ||
            maelys_mcp_json_string_has_nul(result->request_state))) {
            if (out_error) *out_error = maelys_mcp_strdup("requestState must be a string");
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        if (result->content || result->structured_content || result->is_error) {
            if (out_error) *out_error = maelys_mcp_strdup(
                "input_required cannot contain a completed tool result");
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        return MAELYS_MCP_OK;
    }
    if (result->type != MAELYS_MCP_PROVIDER_RESULT_COMPLETE ||
        (!result->content && !result->structured_content)) {
        if (out_error) *out_error = maelys_mcp_strdup(
            "complete provider result requires content or structuredContent");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    if (result->content) {
        maelys_mcp_result_t status = maelys_mcp_validate_content(
            result->content, runtime->max_message_bytes, out_error);
        if (status != MAELYS_MCP_OK) return status;
    }
    if (tool->output_schema) {
        if (!result->structured_content) {
            if (out_error) *out_error = maelys_mcp_strdup(
                "tool with outputSchema requires structuredContent");
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        return maelys_mcp_validate_schema(
            tool->output_schema, result->structured_content, out_error);
    }
    return MAELYS_MCP_OK;
}

static json_t *serialize_provider_result(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_module_request_t *request,
    maelys_mcp_provider_result_t *provider_result) {
    json_t *result = json_object();
    json_t *generated = NULL;
    if (!result) goto failed;
    if (provider_result->type == MAELYS_MCP_PROVIDER_RESULT_INPUT_REQUIRED) {
        if (provider_result->input_requests &&
            json_object_set(result, "inputRequests", provider_result->input_requests) != 0) goto failed;
        if (provider_result->request_state &&
            json_object_set(result, "requestState", provider_result->request_state) != 0) goto failed;
        if (maelys_mcp_add_server_meta(runtime, result, "input_required") != 0) goto failed;
        return maelys_mcp_success_response(request->id, result);
    }
    if (!provider_result->content) {
        char *encoded = json_dumps(provider_result->structured_content,
            JSON_COMPACT | JSON_SORT_KEYS);
        if (!encoded) goto failed;
        generated = text_content(encoded);
        free(encoded);
        if (!generated) goto failed;
    }
    if (json_object_set(result, "content",
            provider_result->content ? provider_result->content : generated) != 0 ||
        (provider_result->structured_content &&
            json_object_set(result, "structuredContent",
                provider_result->structured_content) != 0) ||
        (provider_result->is_error &&
            json_object_set_new(result, "isError", json_true()) != 0) ||
        (request->modern &&
            maelys_mcp_add_server_meta(runtime, result, "complete") != 0)) {
        goto failed;
    }
    if (generated) json_decref(generated);
    return maelys_mcp_success_response(request->id, result);
failed:
    if (generated) json_decref(generated);
    if (result) json_decref(result);
    return maelys_mcp_error_response(request->id, JSONRPC_INTERNAL_ERROR,
        "Internal error", NULL);
}

static json_t *call_tool(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_module_request_t *request) {
    json_t *params = request->params;
    json_t *name = json_is_object(params) ? json_object_get(params, "name") : NULL;
    json_t *arguments = json_is_object(params) ? json_object_get(params, "arguments") : NULL;
    json_t *input_responses = json_is_object(params) ?
        json_object_get(params, "inputResponses") : NULL;
    json_t *request_state = json_is_object(params) ?
        json_object_get(params, "requestState") : NULL;
    /*
     * Capability declaration is per-era, not just per-request: modern clients
     * declare it in _meta on every request, legacy clients declare it once at
     * initialize() and it is snapshotted onto the channel. Both use the same
     * top-level keys (elicitation, sampling, roots), so validate_input_requests
     * needs no era knowledge — only the right source for this call.
     */
    json_t *meta = json_is_object(params) ? json_object_get(params, "_meta") : NULL;
    json_t *client_capabilities = request->modern ?
        (json_is_object(meta) ?
            json_object_get(meta, "io.modelcontextprotocol/clientCapabilities") : NULL) :
        request->legacy_capabilities;
    int invalid_input_responses = 0;
    if (json_is_object(input_responses)) {
        const char *response_key;
        json_t *response_value;
        json_object_foreach(input_responses, response_key, response_value) {
            if (!*response_key || !json_is_object(response_value)) {
                invalid_input_responses = 1;
                break;
            }
        }
    }
    if (!json_is_string(name) || maelys_mcp_json_string_has_nul(name) ||
        (arguments && !json_is_object(arguments)) ||
        (input_responses && !json_is_object(input_responses)) ||
        invalid_input_responses ||
        (request_state && (!json_is_string(request_state) ||
            json_string_length(request_state) == 0u ||
            maelys_mcp_json_string_has_nul(request_state))) ||
        (!maelys_mcp_runtime_module_enabled(runtime, MAELYS_MCP_MODULE_MRTR) &&
            (input_responses || request_state))) {
        return maelys_mcp_error_response(request->id, JSONRPC_INVALID_PARAMS,
            "Invalid tool call", NULL);
    }
    maelys_mcp_owned_tool_t *tool = find_tool(runtime, json_string_value(name));
    if (!tool) return maelys_mcp_error_response(request->id,
        JSONRPC_INVALID_PARAMS, "Unknown tool", NULL);
    json_t *owned_arguments = NULL;
    if (!arguments) {
        owned_arguments = json_object();
        arguments = owned_arguments;
    }
    char *error = NULL;
    maelys_mcp_result_t status = maelys_mcp_validate_schema(
        tool->input_schema, arguments, &error);
    if (status != MAELYS_MCP_OK) {
        json_t *data = json_pack("{s:s}", "detail",
            error ? error : "schema validation failed");
        free(error);
        if (owned_arguments) json_decref(owned_arguments);
        json_t *response = maelys_mcp_error_response(request->id,
            JSONRPC_INVALID_PARAMS, "Invalid tool arguments", data);
        if (data) json_decref(data);
        return response;
    }
    if (!policy_allows(runtime, request, tool, MAELYS_MCP_OPERATION_CALL)) {
        audit(runtime, request, tool, MAELYS_MCP_ERR_DENIED);
        if (owned_arguments) json_decref(owned_arguments);
        return maelys_mcp_error_response(request->id, MCP_POLICY_DENIED,
            "Policy denied", NULL);
    }
    /*
     * params._meta.progressToken is a bare key in both eras - the modern
     * _meta namespaces its own fields but not this one - so a single lookup
     * serves legacy and modern alike. The spec types it as string or
     * integer; anything else is ignored rather than rejected, since progress
     * is advisory and must not fail an otherwise valid call. The reporter
     * lives on this stack frame and is handed to exactly one callback.
     */
    json_t *progress_token = json_is_object(meta) ?
        json_object_get(meta, "progressToken") : NULL;
    int token_usable = (json_is_string(progress_token) &&
            !maelys_mcp_json_string_has_nul(progress_token)) ||
        json_is_integer(progress_token);
    maelys_mcp_progress_reporter_t progress_reporter = {
        .sink = request->sink,
        .token = progress_token
    };
    maelys_mcp_provider_request_t provider_request = {
        .tool_name = tool->name,
        .arguments = arguments,
        .input_responses = input_responses,
        .request_state = request_state,
        .client_capabilities = client_capabilities,
        .progress = (token_usable && request->sink && request->sink->emit) ?
            &progress_reporter : NULL
    };
    maelys_mcp_provider_result_t provider_result;
    maelys_mcp_provider_result_init(&provider_result);
    status = tool->provider->call(tool->provider->context, &provider_request,
        &provider_result, &error);
    if (owned_arguments) json_decref(owned_arguments);
    json_t *required_capabilities = NULL;
    if (status == MAELYS_MCP_OK) {
        status = validate_provider_result(runtime, tool, &provider_result,
            client_capabilities, &required_capabilities, &error);
    }
    audit(runtime, request, tool, status);
    json_t *response;
    if (required_capabilities) {
        json_t *data = json_object();
        if (data) (void)json_object_set(data, "requiredCapabilities", required_capabilities);
        response = maelys_mcp_error_response(request->id,
            MCP_MISSING_REQUIRED_CLIENT_CAPABILITY,
            "Client did not declare a required capability", data);
        if (data) json_decref(data);
        json_decref(required_capabilities);
    } else {
        response = status == MAELYS_MCP_OK ?
            serialize_provider_result(runtime, request, &provider_result) :
            provider_failure(runtime, request, error);
    }
    maelys_mcp_provider_result_clear(&provider_result);
    free(error);
    return response;
}

static int handles(const char *method) {
    return strcmp(method, "tools/list") == 0 || strcmp(method, "tools/call") == 0;
}

static json_t *handle(
    maelys_mcp_runtime_t *runtime,
    const char *method,
    const maelys_mcp_module_request_t *request) {
    return strcmp(method, "tools/list") == 0 ?
        list_tools(runtime, request) : call_tool(runtime, request);
}

static json_t *capability(const maelys_mcp_runtime_t *runtime, int modern) {
    return json_pack("{s:b}", "listChanged",
        modern && maelys_mcp_runtime_module_enabled(
            runtime, MAELYS_MCP_MODULE_SUBSCRIPTIONS));
}

const maelys_mcp_module_descriptor_t maelys_mcp_tools_module = {
    .kind = MAELYS_MCP_MODULE_TOOLS,
    .name = "tools",
    .capability_name = "tools",
    .capability = capability,
    .handles = handles,
    .handle = handle
};
