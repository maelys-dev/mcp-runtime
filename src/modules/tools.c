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

/*
 * Hook 2 for the tools surface. The chain decides on the resolved tool, never
 * on the name the client wrote, so no future rename can route around a deny.
 * Whole params rather than just arguments: inputResponses and requestState
 * are their siblings, and a policy blind to continuation traffic is not a
 * policy over this protocol.
 */
static maelys_mcp_authorize_decision_t authorize_tool(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_module_request_t *request,
    const maelys_mcp_owned_tool_t *tool,
    maelys_mcp_operation_t operation) {
    if (!maelys_mcp_chain_has_authorize(runtime)) return MAELYS_MCP_AUTHORIZE_ALLOW;
    maelys_mcp_authorize_context_t context = {
        .operation = operation,
        .effect = tool ? tool->effect : MAELYS_MCP_EFFECT_UNSPECIFIED,
        .tool_name = tool ? tool->name : NULL,
        .protocol_version = request->protocol_version,
        .client_name = request->client_name,
        .channel = request->channel,
        .params = request->params
    };
    return maelys_mcp_chain_authorize(runtime, &context);
}

/*
 * Hook 5. `requested_name` is the string the client sent and `tool` is what
 * resolved from it; they are equal until hook 1 exists, and the journal is
 * written against the pair rather than against one of them so that they may
 * diverge later without every audit sink changing meaning.
 */
static void audit_tool(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_module_request_t *request,
    const char *requested_name,
    const maelys_mcp_owned_tool_t *tool,
    maelys_mcp_result_t outcome) {
    if (!maelys_mcp_chain_has_audit(runtime)) return;
    maelys_mcp_audit_context_t record = {
        .operation = MAELYS_MCP_OPERATION_CALL,
        .effect = tool ? tool->effect : MAELYS_MCP_EFFECT_UNSPECIFIED,
        .requested_tool_name = requested_name,
        .tool_name = tool ? tool->name : NULL,
        .protocol_version = request->protocol_version,
        .client_name = request->client_name,
        .channel = request->channel,
        .params = request->params,
        .outcome = outcome
    };
    maelys_mcp_chain_audit(runtime, &record);
}

/*
 * The schemas are deep-copied, not referenced. The registry co-owns the
 * originals for the life of the runtime, and this entry leaves for a response
 * whichever thread drains that channel's outbox will release - while other
 * channels' dispatches are still taking and dropping references to the same
 * nodes. Nothing orders those threads, so a shared node is the same
 * refcount race as the transport's shared id and progressToken were: a frame
 * handed to the outbox shares no node with anything another thread will
 * release.
 */
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
        json_object_set_new(root, "inputSchema",
            json_deep_copy(tool->input_schema)) != 0 ||
        (tool->output_schema &&
            json_object_set_new(root, "outputSchema",
                json_deep_copy(tool->output_schema)) != 0) ||
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
    const char *chain_failure = NULL;
    if (!result || !tools) goto failed;
    for (size_t provider_index = 0; provider_index < runtime->provider_count; ++provider_index) {
        maelys_mcp_provider_t *provider = runtime->providers[provider_index];
        for (size_t tool_index = 0; tool_index < provider->tool_count; ++tool_index) {
            maelys_mcp_owned_tool_t *tool = &provider->tools[tool_index];
            maelys_mcp_authorize_decision_t decision = authorize_tool(runtime,
                request, tool, MAELYS_MCP_OPERATION_LIST);
            /*
             * A chain that could not reach a verdict must not quietly shorten
             * the catalog: a hidden entry and an unevaluated one are
             * indistinguishable to the client, so only a real deny omits one.
             */
            if (decision == MAELYS_MCP_AUTHORIZE_ERROR) {
                chain_failure = "Policy evaluation failed";
                goto failed;
            }
            if (decision != MAELYS_MCP_AUTHORIZE_ALLOW) continue;
            json_t *value = tool_as_json(tool);
            if (!value || json_array_append(tools, value) != 0) {
                if (value) json_decref(value);
                goto failed;
            }
            json_decref(value);
        }
    }
    /*
     * Hook 7, after hook 2 has filtered: a denied tool is already gone and
     * cannot be transformed back into view, and what a middleware renames,
     * reschemas or invents here is what the client is told exists. Calls to it
     * come back through hook 1.
     */
    if (maelys_mcp_chain_has_list(runtime)) {
        maelys_mcp_list_context_t list_context = {
            .catalog = MAELYS_MCP_CATALOG_TOOLS,
            .entries = tools,
            .params = request->params,
            .protocol_version = request->protocol_version,
            .client_name = request->client_name,
            .channel = request->channel
        };
        json_t *transformed = NULL;
        if (maelys_mcp_chain_list(runtime, &list_context, &transformed) !=
            MAELYS_MCP_OK) {
            chain_failure = "Catalog transformation failed";
            goto failed;
        }
        if (transformed) {
            json_decref(tools);
            tools = transformed;
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
        chain_failure ? chain_failure : "Internal error", NULL);
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

/*
 * `enforce_output_schema` is what separates the two callers. The provider's
 * own result is checked in full, output schema included: a provider that
 * breaks its declared contract is caught whatever the chain does. A hook 4
 * replacement is checked for everything except that schema, because emitting
 * a value the published schema no longer describes is what redaction *is* -
 * see docs/middleware.md. It still has to be a result this runtime can
 * serialize, and its content blocks still face the wire checks, so a middleware
 * cannot smuggle an oversized or malformed block past validation.
 */
static maelys_mcp_result_t validate_provider_result(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_owned_tool_t *tool,
    const maelys_mcp_provider_result_t *result,
    json_t *client_capabilities,
    int enforce_output_schema,
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
    if (tool->output_schema && enforce_output_schema) {
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
    const char *requested_name = json_string_value(name);
    /*
     * One owned reference for the arguments from here on, whether they came
     * from the client, were absent, or were rewritten by hook 1. A call that
     * carried none becomes an empty object before the chain sees it, so a hook
     * injecting a value never has to special-case "no arguments at all".
     */
    json_t *effective_arguments = arguments ?
        json_incref(arguments) : json_object();
    if (!effective_arguments) {
        return maelys_mcp_error_response(request->id, JSONRPC_INTERNAL_ERROR,
            "Internal error", NULL);
    }
    /*
     * Hook 1, before anything resolves. The registry is deliberately not
     * consulted on the client's spelling first: a name hook 7 published, or
     * one a proxy invented, has no registry entry of its own and would 404
     * before the chain ever got the chance to map it.
     */
    const char *resolved_name = requested_name;
    char *renamed = NULL;
    if (maelys_mcp_chain_has_resolve(runtime)) {
        maelys_mcp_resolve_context_t resolve_context = {
            .operation = MAELYS_MCP_OPERATION_CALL,
            .tool_name = requested_name,
            .arguments = effective_arguments,
            .params = params,
            .protocol_version = request->protocol_version,
            .client_name = request->client_name,
            .channel = request->channel
        };
        json_t *rewritten = NULL;
        if (maelys_mcp_chain_resolve(runtime, &resolve_context, &renamed,
            &rewritten) != MAELYS_MCP_OK) {
            json_decref(effective_arguments);
            audit_tool(runtime, request, requested_name, NULL,
                MAELYS_MCP_ERR_STATE);
            return maelys_mcp_error_response(request->id, JSONRPC_INTERNAL_ERROR,
                "Request transformation failed", NULL);
        }
        if (renamed) resolved_name = renamed;
        if (rewritten) {
            json_decref(effective_arguments);
            effective_arguments = rewritten;
        }
    }
    maelys_mcp_owned_tool_t *tool = find_tool(runtime, resolved_name);
    if (!tool) {
        json_decref(effective_arguments);
        free(renamed);
        return maelys_mcp_error_response(request->id,
            JSONRPC_INVALID_PARAMS, "Unknown tool", NULL);
    }
    /*
     * Policy runs on what hook 1 resolved, and ahead of schema validation.
     * Both halves are deliberate. Deciding after resolution is what stops a
     * rename being a privilege escalation - re-exposing an apply-effect tool
     * under a gentler name must still be seen as an apply. Deciding before
     * validation is the named reorder from the pre-chain runtime: a caller who
     * may not use this tool must not be able to map its argument schema by
     * reading validation error details. The cost is that hook 2 sees params
     * this runtime has not validated yet, which the public contract states.
     */
    maelys_mcp_authorize_decision_t decision = authorize_tool(runtime, request,
        tool, MAELYS_MCP_OPERATION_CALL);
    if (decision != MAELYS_MCP_AUTHORIZE_ALLOW) {
        int failed = decision == MAELYS_MCP_AUTHORIZE_ERROR;
        audit_tool(runtime, request, requested_name, tool,
            failed ? MAELYS_MCP_ERR_STATE : MAELYS_MCP_ERR_DENIED);
        json_decref(effective_arguments);
        free(renamed);
        return failed ?
            maelys_mcp_error_response(request->id, JSONRPC_INTERNAL_ERROR,
                "Policy evaluation failed", NULL) :
            maelys_mcp_error_response(request->id, MCP_POLICY_DENIED,
                "Policy denied", NULL);
    }
    char *error = NULL;
    /*
     * Against the REAL tool's schema, never the one hook 7 published. That is
     * why a transform changing an argument's type has to convert it in hook 1:
     * relabelling it in the catalog alone makes every call fail here.
     */
    maelys_mcp_result_t status = maelys_mcp_validate_schema(
        tool->input_schema, effective_arguments, &error);
    if (status != MAELYS_MCP_OK) {
        json_t *data = json_pack("{s:s}", "detail",
            error ? error : "schema validation failed");
        free(error);
        json_decref(effective_arguments);
        free(renamed);
        json_t *response = maelys_mcp_error_response(request->id,
            JSONRPC_INVALID_PARAMS, "Invalid tool arguments", data);
        if (data) json_decref(data);
        return response;
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
        .arguments = effective_arguments,
        .input_responses = input_responses,
        .request_state = request_state,
        .client_capabilities = client_capabilities,
        .progress = (token_usable && request->sink && request->sink->emit) ?
            &progress_reporter : NULL
    };
    maelys_mcp_provider_result_t provider_result;
    maelys_mcp_provider_result_init(&provider_result);
    /*
     * Hook 3: invoke or substitute. A substituting middleware stands in for
     * the provider completely - the provider is not called, and the result it
     * returns faces the same validation the provider's would have, output
     * schema included, because a cache or a proxy answering in the provider's
     * name answers to the provider's contract.
     */
    int substituted = 0;
    if (maelys_mcp_chain_has_call(runtime)) {
        maelys_mcp_call_context_t call_context = {
            .tool_name = tool->name,
            .requested_tool_name = requested_name,
            .effect = tool->effect,
            .arguments = effective_arguments,
            .params = params,
            .protocol_version = request->protocol_version,
            .client_name = request->client_name,
            .channel = request->channel
        };
        maelys_mcp_call_disposition_t disposition = maelys_mcp_chain_call(
            runtime, &call_context, &provider_result, &error);
        if (disposition == MAELYS_MCP_CALL_ERROR) {
            audit_tool(runtime, request, requested_name, tool,
                MAELYS_MCP_ERR_STATE);
            maelys_mcp_provider_result_clear(&provider_result);
            json_decref(effective_arguments);
            free(renamed);
            json_t *response = maelys_mcp_error_response(request->id,
                JSONRPC_INTERNAL_ERROR,
                error ? error : "Call substitution failed", NULL);
            free(error);
            return response;
        }
        substituted = disposition == MAELYS_MCP_CALL_SUBSTITUTE;
    }
    status = substituted ? MAELYS_MCP_OK :
        tool->provider->call(tool->provider->context, &provider_request,
            &provider_result, &error);
    json_t *required_capabilities = NULL;
    if (status == MAELYS_MCP_OK) {
        status = validate_provider_result(runtime, tool, &provider_result,
            client_capabilities, 1, &required_capabilities, &error);
    }
    /*
     * Hook 4, on a result that has already been checked against the real
     * schema. A replacement is re-checked structurally but not against that
     * schema; see validate_provider_result.
     */
    if (status == MAELYS_MCP_OK && !required_capabilities &&
        maelys_mcp_chain_has_result(runtime)) {
        maelys_mcp_result_context_t result_context = {
            .tool_name = tool->name,
            .requested_tool_name = requested_name,
            .effect = tool->effect,
            .arguments = effective_arguments,
            .params = params,
            .protocol_version = request->protocol_version,
            .client_name = request->client_name,
            .channel = request->channel,
            .result = &provider_result,
            .output_schema = tool->output_schema
        };
        maelys_mcp_result_disposition_t disposition = maelys_mcp_chain_result(
            runtime, &result_context, &provider_result, &error);
        if (disposition == MAELYS_MCP_RESULT_ERROR) {
            status = MAELYS_MCP_ERR_STATE;
            if (!error) error = maelys_mcp_strdup("Result transformation failed");
        } else if (disposition == MAELYS_MCP_RESULT_REPLACED) {
            status = validate_provider_result(runtime, tool, &provider_result,
                client_capabilities, 0, &required_capabilities, &error);
        }
    }
    json_decref(effective_arguments);
    free(renamed);
    audit_tool(runtime, request, requested_name, tool, status);
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
