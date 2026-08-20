#include "src/internal/internal.h"
#include "maelys/mcp/uri.h"

#include <stdlib.h>
#include <string.h>

/*
 * Hook 2 for the resource surface. Three of the runtime's five decision
 * points are here, which is why the chain could not ship for tools alone:
 * with the chain replacing the old callbacks, a tools-only conversion would
 * have removed resource authorization outright.
 *
 * The URI passed in is always the canonical one for a read - parsed and
 * normalized before the decision - so a policy cannot be evaded by spelling a
 * URI differently. Catalog entries are already normalized at registration.
 */
static maelys_mcp_authorize_decision_t authorize_resource(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_module_request_t *request,
    maelys_mcp_operation_t operation,
    const char *uri) {
    if (!maelys_mcp_chain_has_authorize(runtime)) return MAELYS_MCP_AUTHORIZE_ALLOW;
    maelys_mcp_authorize_context_t context = {
        .operation = operation,
        /* Reading is the only effect this surface has. */
        .effect = MAELYS_MCP_EFFECT_READ,
        .resource_uri = uri,
        .protocol_version = request->protocol_version,
        .client_name = request->client_name,
        .channel = request->channel,
        .params = request->params
    };
    return maelys_mcp_chain_authorize(runtime, &context);
}

/*
 * Hook 5. `requested_uri` is the string the client sent, `uri` the canonical
 * form that was read; unlike the tools path these two genuinely differ today,
 * whenever normalization rewrites the URI.
 */
static void audit_resource(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_module_request_t *request,
    const char *requested_uri,
    const char *uri,
    maelys_mcp_result_t outcome) {
    if (!maelys_mcp_chain_has_audit(runtime)) return;
    maelys_mcp_audit_context_t record = {
        .operation = MAELYS_MCP_OPERATION_RESOURCE_READ,
        .effect = MAELYS_MCP_EFFECT_READ,
        .requested_resource_uri = requested_uri,
        .resource_uri = uri,
        .protocol_version = request->protocol_version,
        .client_name = request->client_name,
        .channel = request->channel,
        .params = request->params,
        .outcome = outcome
    };
    maelys_mcp_chain_audit(runtime, &record);
}

/*
 * Hook 7 for the two resource catalogs. Returns zero when the chain failed,
 * and otherwise leaves *entries holding whatever the catalog is now - the
 * original array, or a replacement the chain owns and this function has taken
 * over.
 */
static int transform_catalog(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_module_request_t *request,
    maelys_mcp_catalog_kind_t catalog,
    json_t **entries) {
    if (!maelys_mcp_chain_has_list(runtime)) return 1;
    maelys_mcp_list_context_t context = {
        .catalog = catalog,
        .entries = *entries,
        .params = request->params,
        .protocol_version = request->protocol_version,
        .client_name = request->client_name,
        .channel = request->channel
    };
    json_t *transformed = NULL;
    if (maelys_mcp_chain_list(runtime, &context, &transformed) != MAELYS_MCP_OK) {
        return 0;
    }
    if (transformed) {
        json_decref(*entries);
        *entries = transformed;
    }
    return 1;
}

static int set_optional_string(json_t *object, const char *name, const char *value) {
    return !value || !*value || json_object_set_new(object, name, json_string(value)) == 0;
}

static json_t *resource_as_json(const maelys_mcp_owned_resource_t *resource) {
    json_t *value = json_object();
    if (!value || json_object_set_new(value, "uri", json_string(resource->uri)) != 0 ||
        json_object_set_new(value, "name", json_string(resource->name)) != 0 ||
        !set_optional_string(value, "title", resource->title) ||
        !set_optional_string(value, "description", resource->description) ||
        !set_optional_string(value, "mimeType", resource->mime_type) ||
        (resource->has_size &&
         json_object_set_new(value, "size", json_integer(resource->size)) != 0)) {
        if (value) json_decref(value);
        return NULL;
    }
    return value;
}

static json_t *template_as_json(const maelys_mcp_owned_resource_template_t *resource) {
    json_t *value = json_object();
    if (!value || json_object_set_new(value, "uriTemplate",
            json_string(resource->uri_template)) != 0 ||
        json_object_set_new(value, "name", json_string(resource->name)) != 0 ||
        !set_optional_string(value, "title", resource->title) ||
        !set_optional_string(value, "description", resource->description) ||
        !set_optional_string(value, "mimeType", resource->mime_type)) {
        if (value) json_decref(value);
        return NULL;
    }
    return value;
}

static int valid_list_params(json_t *params) {
    if (!params) return 1;
    if (!json_is_object(params)) return 0;
    json_t *cursor = json_object_get(params, "cursor");
    return !cursor;
}

static json_t *list_resources(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_module_request_t *request) {
    if (!valid_list_params(request->params)) {
        return maelys_mcp_error_response(request->id, JSONRPC_INVALID_PARAMS,
            "Resources cursor is invalid", NULL);
    }
    json_t *result = json_object();
    json_t *resources = json_array();
    const char *chain_failure = NULL;
    if (!result || !resources) goto failed;
    for (size_t provider_index = 0; provider_index < runtime->provider_count; ++provider_index) {
        maelys_mcp_provider_t *provider = runtime->providers[provider_index];
        for (size_t index = 0; index < provider->resource_count; ++index) {
            maelys_mcp_owned_resource_t *resource = &provider->resources[index];
            maelys_mcp_authorize_decision_t decision = authorize_resource(runtime,
                request, MAELYS_MCP_OPERATION_RESOURCE_LIST, resource->uri);
            /* Only a real deny hides a catalog entry; see list_tools. */
            if (decision == MAELYS_MCP_AUTHORIZE_ERROR) {
                chain_failure = "Policy evaluation failed";
                goto failed;
            }
            if (decision != MAELYS_MCP_AUTHORIZE_ALLOW) continue;
            json_t *value = resource_as_json(resource);
            if (!value || json_array_append_new(resources, value) != 0) goto failed;
        }
    }
    /* Hook 7, on the resource catalog; see list_tools for what it may and may
     * not undo. */
    if (!transform_catalog(runtime, request, MAELYS_MCP_CATALOG_RESOURCES,
        &resources)) {
        chain_failure = "Catalog transformation failed";
        goto failed;
    }
    if (json_object_set_new(result, "resources", resources) != 0) goto failed;
    resources = NULL;
    if (request->modern &&
        (json_object_set_new(result, "ttlMs", json_integer(0)) != 0 ||
         json_object_set_new(result, "cacheScope", json_string("private")) != 0 ||
         maelys_mcp_add_server_meta(runtime, result, "complete") != 0)) goto failed;
    return maelys_mcp_success_response(request->id, result);
failed:
    if (resources) json_decref(resources);
    if (result) json_decref(result);
    return maelys_mcp_error_response(request->id, JSONRPC_INTERNAL_ERROR,
        chain_failure ? chain_failure : "Internal error", NULL);
}

static json_t *list_templates(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_module_request_t *request) {
    if (!valid_list_params(request->params)) {
        return maelys_mcp_error_response(request->id, JSONRPC_INVALID_PARAMS,
            "Resource template cursor is invalid", NULL);
    }
    json_t *result = json_object();
    json_t *templates = json_array();
    const char *chain_failure = NULL;
    if (!result || !templates) goto failed;
    for (size_t provider_index = 0; provider_index < runtime->provider_count; ++provider_index) {
        maelys_mcp_provider_t *provider = runtime->providers[provider_index];
        for (size_t index = 0; index < provider->resource_template_count; ++index) {
            maelys_mcp_owned_resource_template_t *resource = &provider->resource_templates[index];
            maelys_mcp_authorize_decision_t decision = authorize_resource(runtime,
                request, MAELYS_MCP_OPERATION_RESOURCE_TEMPLATE_LIST,
                resource->uri_template);
            /* Only a real deny hides a catalog entry; see list_tools. */
            if (decision == MAELYS_MCP_AUTHORIZE_ERROR) {
                chain_failure = "Policy evaluation failed";
                goto failed;
            }
            if (decision != MAELYS_MCP_AUTHORIZE_ALLOW) continue;
            json_t *value = template_as_json(resource);
            if (!value || json_array_append_new(templates, value) != 0) goto failed;
        }
    }
    if (!transform_catalog(runtime, request,
        MAELYS_MCP_CATALOG_RESOURCE_TEMPLATES, &templates)) {
        chain_failure = "Catalog transformation failed";
        goto failed;
    }
    if (json_object_set_new(result, "resourceTemplates", templates) != 0) goto failed;
    templates = NULL;
    if (request->modern &&
        (json_object_set_new(result, "ttlMs", json_integer(0)) != 0 ||
         json_object_set_new(result, "cacheScope", json_string("private")) != 0 ||
         maelys_mcp_add_server_meta(runtime, result, "complete") != 0)) goto failed;
    return maelys_mcp_success_response(request->id, result);
failed:
    if (templates) json_decref(templates);
    if (result) json_decref(result);
    return maelys_mcp_error_response(request->id, JSONRPC_INTERNAL_ERROR,
        chain_failure ? chain_failure : "Internal error", NULL);
}

static maelys_mcp_provider_t *exact_provider(
    maelys_mcp_runtime_t *runtime,
    const char *uri) {
    for (size_t provider_index = 0; provider_index < runtime->provider_count; ++provider_index) {
        maelys_mcp_provider_t *provider = runtime->providers[provider_index];
        for (size_t index = 0; index < provider->resource_count; ++index) {
            if (strcmp(provider->resources[index].uri, uri) == 0) return provider;
        }
    }
    return NULL;
}

static int provider_has_templates(const maelys_mcp_provider_t *provider) {
    return provider && provider->resource_template_count && provider->read_resource;
}

/*
 * Unlike tools.c's call_tool, resources/read has never validated which
 * specific capability (roots/elicitation/sampling) a client declared before
 * honoring input_required here - only whether MRTR is enabled at all. That
 * pre-existing, modern-and-legacy-alike simplification is out of scope for
 * this fix. What was legacy-specific (and wrong) was gating input_required
 * on request->modern unconditionally, denying it to every legacy channel
 * regardless of what it declared at initialize - dropped below.
 */
static int valid_input_required(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_resource_result_t *result) {
    return maelys_mcp_runtime_module_enabled(runtime, MAELYS_MCP_MODULE_MRTR) &&
        ((json_is_object(result->input_requests) && json_object_size(result->input_requests)) ||
         (json_is_string(result->request_state) &&
          !maelys_mcp_json_string_has_nul(result->request_state)));
}

static json_t *serialize_read_result(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_module_request_t *request,
    maelys_mcp_resource_result_t *provider_result) {
    json_t *result = json_object();
    if (!result) goto failed;
    if (provider_result->type == MAELYS_MCP_RESOURCE_RESULT_INPUT_REQUIRED) {
        if (!valid_input_required(runtime, provider_result)) goto failed;
        if (provider_result->input_requests && json_object_set(result,
            "inputRequests", provider_result->input_requests) != 0) goto failed;
        if (provider_result->request_state && json_object_set(result,
            "requestState", provider_result->request_state) != 0) goto failed;
        if (maelys_mcp_add_server_meta(runtime, result, "input_required") != 0) goto failed;
        return maelys_mcp_success_response(request->id, result);
    }
    if (provider_result->type != MAELYS_MCP_RESOURCE_RESULT_COMPLETE ||
        maelys_mcp_validate_resource_contents(provider_result->contents,
            runtime->max_message_bytes, NULL) != MAELYS_MCP_OK ||
        json_object_set(result, "contents", provider_result->contents) != 0) goto failed;
    if (request->modern &&
        (json_object_set_new(result, "ttlMs", json_integer(0)) != 0 ||
         json_object_set_new(result, "cacheScope", json_string("private")) != 0 ||
         maelys_mcp_add_server_meta(runtime, result, "complete") != 0)) goto failed;
    return maelys_mcp_success_response(request->id, result);
failed:
    if (result) json_decref(result);
    return maelys_mcp_error_response(request->id, JSONRPC_INTERNAL_ERROR,
        "Invalid resource provider result", NULL);
}

static json_t *read_resource(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_module_request_t *request) {
    json_t *uri_value = json_is_object(request->params) ?
        json_object_get(request->params, "uri") : NULL;
    json_t *input_responses = json_is_object(request->params) ?
        json_object_get(request->params, "inputResponses") : NULL;
    json_t *request_state = json_is_object(request->params) ?
        json_object_get(request->params, "requestState") : NULL;
    if (!json_is_string(uri_value) || maelys_mcp_json_string_has_nul(uri_value) ||
        (input_responses && !json_is_object(input_responses)) ||
        (request_state && (!json_is_string(request_state) ||
            maelys_mcp_json_string_has_nul(request_state)))) {
        return maelys_mcp_error_response(request->id, JSONRPC_INVALID_PARAMS,
            "Invalid resource read request", NULL);
    }
    maelys_uri_options_t options = {.max_bytes = 8192u, .require_scheme = 1};
    maelys_uri_t *parsed = NULL;
    char *error = NULL;
    if (maelys_uri_parse(json_string_value(uri_value), json_string_length(uri_value),
        &options, &parsed, &error) != MAELYS_MCP_OK) {
        json_t *data = error ? json_pack("{s:s}", "detail", error) : NULL;
        free(error);
        json_t *response = maelys_mcp_error_response(request->id,
            JSONRPC_INVALID_PARAMS, "Invalid resource URI", data);
        if (data) json_decref(data);
        return response;
    }
    const char *canonical = maelys_uri_canonical(parsed);
    const char *requested_uri = json_string_value(uri_value);
    maelys_mcp_authorize_decision_t decision = authorize_resource(runtime,
        request, MAELYS_MCP_OPERATION_RESOURCE_READ, canonical);
    if (decision != MAELYS_MCP_AUTHORIZE_ALLOW) {
        /*
         * A denied read is journalled, which a denied tool call already was
         * and this path was not - the asymmetry was an omission, not a
         * decision, and a policy surface whose refusals leave no trace is the
         * wrong half to leave untouched.
         */
        int failed = decision == MAELYS_MCP_AUTHORIZE_ERROR;
        audit_resource(runtime, request, requested_uri, canonical,
            failed ? MAELYS_MCP_ERR_STATE : MAELYS_MCP_ERR_DENIED);
        maelys_uri_destroy(parsed);
        return failed ?
            maelys_mcp_error_response(request->id, JSONRPC_INTERNAL_ERROR,
                "Policy evaluation failed", NULL) :
            maelys_mcp_error_response(request->id, MCP_POLICY_DENIED,
                "Policy denied", NULL);
    }
    /*
     * Same era-aware source as tools.c's call_tool: modern clients declare
     * capabilities per-request in _meta, legacy clients declare them once at
     * initialize and mcp-runtime snapshots that onto every request on the
     * channel. Resolve to whichever the request's era actually carries so a
     * real out-of-process provider can see what the client declared,
     * regardless of protocol era.
     */
    json_t *meta = json_object_get(request->params, "_meta");
    json_t *client_capabilities = request->modern ?
        (json_is_object(meta) ?
            json_object_get(meta, "io.modelcontextprotocol/clientCapabilities") : NULL) :
        request->legacy_capabilities;
    maelys_mcp_resource_request_t provider_request = {
        .uri = canonical,
        .input_responses = input_responses,
        .request_state = request_state,
        .client_capabilities = client_capabilities
    };
    maelys_mcp_provider_t *provider = exact_provider(runtime, canonical);
    maelys_mcp_resource_result_t provider_result;
    maelys_mcp_resource_result_init(&provider_result);
    maelys_mcp_result_t status = MAELYS_MCP_ERR_NOT_FOUND;
    if (provider && provider->read_resource) {
        status = provider->read_resource(provider->context, &provider_request,
            &provider_result, &error);
    } else {
        for (size_t index = 0; index < runtime->provider_count; ++index) {
            provider = runtime->providers[index];
            if (!provider_has_templates(provider)) continue;
            status = provider->read_resource(provider->context, &provider_request,
                &provider_result, &error);
            if (status != MAELYS_MCP_ERR_NOT_FOUND) break;
            free(error);
            error = NULL;
            maelys_mcp_resource_result_clear(&provider_result);
        }
    }
    audit_resource(runtime, request, requested_uri, canonical, status);
    json_t *error_data = status == MAELYS_MCP_ERR_NOT_FOUND ?
        json_pack("{s:s}", "uri", canonical) : NULL;
    maelys_uri_destroy(parsed);
    json_t *response = status == MAELYS_MCP_OK ?
        serialize_read_result(runtime, request, &provider_result) :
        maelys_mcp_error_response(request->id,
            status == MAELYS_MCP_ERR_NOT_FOUND ? JSONRPC_INVALID_PARAMS : JSONRPC_INTERNAL_ERROR,
            error ? error : "Resource not found", error_data);
    if (error_data) json_decref(error_data);
    maelys_mcp_resource_result_clear(&provider_result);
    free(error);
    return response;
}

static json_t *capability(const maelys_mcp_runtime_t *runtime, int modern) {
    int subscriptions = modern && maelys_mcp_runtime_module_enabled(
        runtime, MAELYS_MCP_MODULE_SUBSCRIPTIONS);
    return json_pack("{s:b,s:b}", "listChanged", subscriptions,
        "subscribe", subscriptions);
}

static int handles(const char *method) {
    return strcmp(method, "resources/list") == 0 ||
        strcmp(method, "resources/templates/list") == 0 ||
        strcmp(method, "resources/read") == 0;
}

static json_t *handle(
    maelys_mcp_runtime_t *runtime,
    const char *method,
    const maelys_mcp_module_request_t *request) {
    if (strcmp(method, "resources/list") == 0) return list_resources(runtime, request);
    if (strcmp(method, "resources/templates/list") == 0) return list_templates(runtime, request);
    return read_resource(runtime, request);
}

const maelys_mcp_module_descriptor_t maelys_mcp_resources_module = {
    .kind = MAELYS_MCP_MODULE_RESOURCES,
    .name = "resources",
    .capability_name = "resources",
    .capability = capability,
    .handles = handles,
    .handle = handle
};
