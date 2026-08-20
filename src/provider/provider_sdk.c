#include "maelys/mcp/provider_sdk.h"
#include "maelys/mcp/uri.h"
#include "src/internal/internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct maelys_mcp_provider_sdk {
    const maelys_mcp_provider_sdk_config_t *config;
    int output_fd;
    int activated;
    int shutting_down;
    int shutdown_called;
    size_t events_inflight;
    pthread_mutex_t write_mutex;
    int write_mutex_initialized;
    pthread_mutex_t state_mutex;
    pthread_cond_t events_idle;
    int state_mutex_initialized;
    int events_idle_initialized;
    /*
     * Set once serve()'s reader exists, borrowed from its stack frame for the
     * duration of the loop. maelys_mcp_provider_sdk_request_client reuses it
     * rather than opening a second reader on the same fd: the serve loop and
     * a call it dispatched run on the same thread, so there is only ever one
     * reader of this connection at a time, and it must stay one buffer so a
     * reply that arrives back-to-back with whatever the host sends next is
     * never split across two.
     */
    maelys_mcp_line_reader_t *reader;
    int input_fd;
    /* Guards against a provider calling the nested-request helper again
     * before the first call returned, which would corrupt the wire's own
     * single-outstanding rule rather than just this SDK's bookkeeping. */
    int nested_inflight;
    unsigned long long nested_sequence;
};

static int set_optional_string(json_t *object, const char *name, const char *value) {
    return !value || !*value || json_object_set_new(object, name, json_string(value)) == 0;
}

static void replace_error(char **out_error, const char *message) {
    if (!out_error) return;
    free(*out_error);
    *out_error = maelys_mcp_strdup(message ? message : "nested request failed");
}

static json_t *tool_description(const maelys_mcp_tool_t *tool) {
    const char *effect = maelys_mcp_tool_effect_string(tool ? tool->effect : MAELYS_MCP_EFFECT_UNSPECIFIED);
    json_t *value = json_object();
    if (!value || !tool || !effect ||
        json_object_set_new(value, "name", json_string(tool->name)) != 0 ||
        json_object_set_new(value, "title", json_string(tool->title ? tool->title : tool->name)) != 0 ||
        json_object_set_new(value, "description", json_string(tool->description)) != 0 ||
        json_object_set(value, "inputSchema", tool->input_schema) != 0 ||
        (tool->output_schema && json_object_set(value, "outputSchema", tool->output_schema) != 0) ||
        json_object_set_new(value, "effect", json_string(effect)) != 0) {
        if (value) json_decref(value);
        return NULL;
    }
    return value;
}

static json_t *resource_description(const maelys_mcp_resource_t *resource) {
    json_t *value = json_object();
    if (!value || !resource ||
        json_object_set_new(value, "uri", json_string(resource->uri)) != 0 ||
        json_object_set_new(value, "name", json_string(resource->name)) != 0 ||
        !set_optional_string(value, "title", resource->title) ||
        !set_optional_string(value, "description", resource->description) ||
        !set_optional_string(value, "mimeType", resource->mime_type) ||
        (resource->has_size && json_object_set_new(value, "size", json_integer(resource->size)) != 0)) {
        if (value) json_decref(value);
        return NULL;
    }
    return value;
}

static json_t *template_description(const maelys_mcp_resource_template_t *resource) {
    json_t *value = json_object();
    if (!value || !resource ||
        json_object_set_new(value, "uriTemplate", json_string(resource->uri_template)) != 0 ||
        json_object_set_new(value, "name", json_string(resource->name)) != 0 ||
        !set_optional_string(value, "title", resource->title) ||
        !set_optional_string(value, "description", resource->description) ||
        !set_optional_string(value, "mimeType", resource->mime_type)) {
        if (value) json_decref(value);
        return NULL;
    }
    return value;
}

static json_t *describe_provider(const maelys_mcp_provider_sdk_config_t *config) {
    json_t *root = json_object();
    json_t *tools = json_array();
    if (!root || !tools ||
        json_object_set_new(root, "name", json_string(config->name)) != 0 ||
        json_object_set_new(root, "version", json_string(config->version)) != 0) {
        if (root) json_decref(root);
        if (tools) json_decref(tools);
        return NULL;
    }
    for (size_t index = 0; index < config->tool_count; ++index) {
        json_t *tool = tool_description(&config->tools[index]);
        if (!tool || json_array_append_new(tools, tool) != 0) {
            if (tool) json_decref(tool);
            json_decref(root);
            json_decref(tools);
            return NULL;
        }
    }
    if (json_object_set(root, "tools", tools) != 0) {
        json_decref(root);
        json_decref(tools);
        return NULL;
    }
    json_decref(tools);
    if (config->resource_count) {
        json_t *resources = json_array();
        if (!resources) {
            json_decref(root);
            return NULL;
        }
        for (size_t index = 0; index < config->resource_count; ++index) {
            json_t *resource = resource_description(&config->resources[index]);
            if (!resource || json_array_append_new(resources, resource) != 0) {
                if (resource) json_decref(resource);
                json_decref(resources);
                json_decref(root);
                return NULL;
            }
        }
        if (json_object_set(root, "resources", resources) != 0) {
            json_decref(resources);
            json_decref(root);
            return NULL;
        }
        json_decref(resources);
    }
    if (config->resource_template_count) {
        json_t *templates = json_array();
        if (!templates) {
            json_decref(root);
            return NULL;
        }
        for (size_t index = 0; index < config->resource_template_count; ++index) {
            json_t *resource = template_description(&config->resource_templates[index]);
            if (!resource || json_array_append_new(templates, resource) != 0) {
                if (resource) json_decref(resource);
                json_decref(templates);
                json_decref(root);
                return NULL;
            }
        }
        if (json_object_set(root, "resourceTemplates", templates) != 0) {
            json_decref(templates);
            json_decref(root);
            return NULL;
        }
        json_decref(templates);
    }
    return root;
}

static int resource_descriptors_valid(const maelys_mcp_provider_sdk_config_t *config) {
    for (size_t index = 0; index < config->resource_count; ++index) {
        const maelys_mcp_resource_t *resource = &config->resources[index];
        if (!resource->uri || !resource->name || !*resource->name ||
            (resource->has_size && resource->size < 0)) return 0;
        maelys_uri_t *uri = NULL;
        maelys_uri_options_t options = {.max_bytes = 8192u, .require_scheme = 1};
        if (maelys_uri_parse(resource->uri, strlen(resource->uri), &options,
                &uri, NULL) != MAELYS_MCP_OK) {
            return 0;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            maelys_uri_t *previous_uri = NULL;
            if (maelys_uri_parse(config->resources[previous].uri,
                    strlen(config->resources[previous].uri), &options,
                    &previous_uri, NULL) != MAELYS_MCP_OK ||
                strcmp(maelys_uri_canonical(uri), maelys_uri_canonical(previous_uri)) == 0) {
                maelys_uri_destroy(previous_uri);
                maelys_uri_destroy(uri);
                return 0;
            }
            maelys_uri_destroy(previous_uri);
        }
        maelys_uri_destroy(uri);
    }
    for (size_t index = 0; index < config->resource_template_count; ++index) {
        const maelys_mcp_resource_template_t *resource = &config->resource_templates[index];
        if (!resource->uri_template || !resource->name || !*resource->name ||
            maelys_uri_template_validate(resource->uri_template,
                strlen(resource->uri_template), 8192u, NULL) != MAELYS_MCP_OK) {
            return 0;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (strcmp(resource->uri_template,
                    config->resource_templates[previous].uri_template) == 0) return 0;
        }
    }
    return 1;
}

static int provider_config_valid(const maelys_mcp_provider_sdk_config_t *config) {
    if (!config || !config->name || !*config->name || !config->version || !*config->version ||
        (config->tool_count && (!config->tools || !config->call)) ||
        (config->resource_count && !config->resources) ||
        (config->resource_template_count && !config->resource_templates) ||
        ((config->resource_count || config->resource_template_count) && !config->read_resource)) {
        return 0;
    }
    for (size_t index = 0; index < config->tool_count; ++index) {
        const maelys_mcp_tool_t *tool = &config->tools[index];
        if (!tool->name || !*tool->name || !tool->description ||
            !json_is_object(tool->input_schema) ||
            !maelys_mcp_tool_effect_string(tool->effect)) return 0;
        if (maelys_mcp_validate_schema_definition(tool->input_schema, 1, NULL) != MAELYS_MCP_OK ||
            (tool->output_schema &&
             maelys_mcp_validate_schema_definition(tool->output_schema, 0, NULL) != MAELYS_MCP_OK)) {
            return 0;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (strcmp(tool->name, config->tools[previous].name) == 0) return 0;
        }
    }
    return resource_descriptors_valid(config);
}

static const maelys_mcp_tool_t *find_tool(
    const maelys_mcp_provider_sdk_config_t *config,
    const char *name) {
    for (size_t index = 0; index < config->tool_count; ++index) {
        if (strcmp(config->tools[index].name, name) == 0) return &config->tools[index];
    }
    return NULL;
}

static maelys_mcp_result_t write_message(maelys_mcp_provider_sdk_t *sdk, json_t *message) {
    if (!sdk || !message) return MAELYS_MCP_ERR_ARGUMENT;
    pthread_mutex_lock(&sdk->write_mutex);
    maelys_mcp_result_t status = maelys_mcp_write_json_line(sdk->output_fd, message);
    pthread_mutex_unlock(&sdk->write_mutex);
    return status;
}

static void begin_shutdown(maelys_mcp_provider_sdk_t *sdk) {
    pthread_mutex_lock(&sdk->state_mutex);
    sdk->shutting_down = 1;
    pthread_mutex_unlock(&sdk->state_mutex);
}

static void wait_for_events(maelys_mcp_provider_sdk_t *sdk) {
    pthread_mutex_lock(&sdk->state_mutex);
    while (sdk->events_inflight != 0u) {
        pthread_cond_wait(&sdk->events_idle, &sdk->state_mutex);
    }
    pthread_mutex_unlock(&sdk->state_mutex);
}

static void shutdown_provider(maelys_mcp_provider_sdk_t *sdk) {
    begin_shutdown(sdk);
    pthread_mutex_lock(&sdk->state_mutex);
    int call_shutdown = !sdk->shutdown_called;
    sdk->shutdown_called = 1;
    pthread_mutex_unlock(&sdk->state_mutex);
    if (call_shutdown && sdk->config->shutdown) sdk->config->shutdown(sdk->config->context);
    wait_for_events(sdk);
}

static json_t *response_envelope(json_t *id) {
    json_t *response = json_object();
    if (!response ||
        json_object_set_new(response, "protocol", json_string(MAELYS_MCP_PROVIDER_PROTOCOL)) != 0 ||
        json_object_set(response, "id", id ? id : json_null()) != 0) {
        if (response) json_decref(response);
        return NULL;
    }
    return response;
}

static json_t *error_response(json_t *id, const char *code, const char *message) {
    json_t *response = response_envelope(id);
    json_t *error = json_object();
    if (!response || !error ||
        json_object_set_new(error, "code", json_string(code ? code : "provider_error")) != 0 ||
        json_object_set_new(error, "message", json_string(message ? message : "provider error")) != 0 ||
        json_object_set_new(response, "error", error) != 0) {
        if (response) json_decref(response);
        if (error) json_decref(error);
        return NULL;
    }
    return response;
}

static json_t *success_response(json_t *id, json_t *result) {
    json_t *response = response_envelope(id);
    if (!response || !result || json_object_set_new(response, "result", result) != 0) {
        if (response) json_decref(response);
        if (result) json_decref(result);
        return NULL;
    }
    return response;
}

static json_t *serialize_tool_result(const maelys_mcp_provider_result_t *result) {
    json_t *wire = json_object();
    if (!wire) return NULL;
    if (result->type == MAELYS_MCP_PROVIDER_RESULT_INPUT_REQUIRED) {
        if ((!result->input_requests || !json_object_size(result->input_requests)) &&
            !json_is_string(result->request_state)) goto failed;
        if (json_object_set_new(wire, "resultType", json_string("input_required")) != 0 ||
            (result->input_requests && json_object_set(wire, "inputRequests", result->input_requests) != 0) ||
            (result->request_state && json_object_set(wire, "requestState", result->request_state) != 0)) {
            goto failed;
        }
        return wire;
    }
    if (result->type != MAELYS_MCP_PROVIDER_RESULT_COMPLETE ||
        (!result->content && !result->structured_content)) {
        goto failed;
    }
    if (json_object_set_new(wire, "resultType", json_string("complete")) != 0 ||
        (result->content && json_object_set(wire, "content", result->content) != 0) ||
        (result->structured_content &&
            json_object_set(wire, "structuredContent", result->structured_content) != 0) ||
        (result->is_error && json_object_set_new(wire, "isError", json_true()) != 0)) {
        goto failed;
    }
    return wire;
failed:
    json_decref(wire);
    return NULL;
}

static json_t *serialize_resource_result(const maelys_mcp_resource_result_t *result) {
    json_t *wire = json_object();
    if (!wire) return NULL;
    if (result->type == MAELYS_MCP_RESOURCE_RESULT_INPUT_REQUIRED) {
        if ((!result->input_requests || !json_object_size(result->input_requests)) &&
            !json_is_string(result->request_state)) goto failed;
        if (json_object_set_new(wire, "resultType", json_string("input_required")) != 0 ||
            (result->input_requests && json_object_set(wire, "inputRequests", result->input_requests) != 0) ||
            (result->request_state && json_object_set(wire, "requestState", result->request_state) != 0)) {
            goto failed;
        }
        return wire;
    }
    if (result->type != MAELYS_MCP_RESOURCE_RESULT_COMPLETE ||
        !json_is_array(result->contents) ||
        json_array_size(result->contents) == 0u) {
        goto failed;
    }
    if (json_object_set_new(wire, "resultType", json_string("complete")) != 0 ||
        json_object_set(wire, "contents", result->contents) != 0) {
        goto failed;
    }
    return wire;
failed:
    json_decref(wire);
    return NULL;
}

static json_t *handle_call(maelys_mcp_provider_sdk_t *sdk, json_t *id, json_t *params) {
    if (!json_is_object(params)) return error_response(id, "provider_error", "invalid provider call");
    json_t *name = json_object_get(params, "name");
    json_t *arguments = json_object_get(params, "arguments");
    json_t *input_responses = json_object_get(params, "inputResponses");
    json_t *request_state = json_object_get(params, "requestState");
    json_t *client_capabilities = json_object_get(params, "clientCapabilities");
    if (!json_is_string(name) || maelys_mcp_json_string_has_nul(name) ||
        (arguments && !json_is_object(arguments)) ||
        (input_responses && !json_is_object(input_responses)) ||
        (request_state && (!json_is_string(request_state) || maelys_mcp_json_string_has_nul(request_state))) ||
        (client_capabilities && !json_is_object(client_capabilities))) {
        return error_response(id, "provider_error", "invalid provider call");
    }
    const maelys_mcp_tool_t *tool = find_tool(sdk->config, json_string_value(name));
    if (!tool) return error_response(id, "provider_error", "unknown tool");
    json_t *empty_arguments = NULL;
    if (!arguments) {
        empty_arguments = json_object();
        arguments = empty_arguments;
        if (!arguments) return error_response(id, "provider_error", "out of memory");
    }
    maelys_mcp_provider_request_t request = {
        .tool_name = json_string_value(name),
        .arguments = arguments,
        .input_responses = input_responses,
        .request_state = request_state,
        .client_capabilities = client_capabilities
    };
    maelys_mcp_provider_result_t result;
    maelys_mcp_provider_result_init(&result);
    char *error = NULL;
    maelys_mcp_result_t status = sdk->config->call(
        sdk, sdk->config->context, &request, &result, &error);
    json_t *wire = status == MAELYS_MCP_OK ? serialize_tool_result(&result) : NULL;
    maelys_mcp_provider_result_clear(&result);
    if (empty_arguments) json_decref(empty_arguments);
    if (status != MAELYS_MCP_OK || !wire) {
        json_t *response = error_response(id, "provider_error",
            error ? error : (wire ? "provider call failed" : "invalid provider result"));
        free(error);
        if (wire) json_decref(wire);
        return response;
    }
    free(error);
    return success_response(id, wire);
}

static json_t *handle_read_resource(maelys_mcp_provider_sdk_t *sdk, json_t *id, json_t *params) {
    if (!sdk->config->read_resource) {
        return error_response(id, "not_found", "provider does not expose resources");
    }
    json_t *uri = json_is_object(params) ? json_object_get(params, "uri") : NULL;
    json_t *input_responses = json_is_object(params) ? json_object_get(params, "inputResponses") : NULL;
    json_t *request_state = json_is_object(params) ? json_object_get(params, "requestState") : NULL;
    json_t *client_capabilities = json_is_object(params) ? json_object_get(params, "clientCapabilities") : NULL;
    if (!json_is_string(uri) || maelys_mcp_json_string_has_nul(uri) ||
        (input_responses && !json_is_object(input_responses)) ||
        (request_state && (!json_is_string(request_state) || maelys_mcp_json_string_has_nul(request_state))) ||
        (client_capabilities && !json_is_object(client_capabilities))) {
        return error_response(id, "provider_error", "invalid resource read");
    }
    maelys_mcp_resource_request_t request = {
        .uri = json_string_value(uri),
        .input_responses = input_responses,
        .request_state = request_state,
        .client_capabilities = client_capabilities
    };
    maelys_mcp_resource_result_t result;
    maelys_mcp_resource_result_init(&result);
    char *error = NULL;
    maelys_mcp_result_t status = sdk->config->read_resource(
        sdk, sdk->config->context, &request, &result, &error);
    json_t *wire = status == MAELYS_MCP_OK ? serialize_resource_result(&result) : NULL;
    maelys_mcp_resource_result_clear(&result);
    if (status != MAELYS_MCP_OK || !wire) {
        const char *code = status == MAELYS_MCP_ERR_NOT_FOUND ? "not_found" : "provider_error";
        json_t *response = error_response(id, code,
            error ? error : (wire ? "resource read failed" : "invalid resource result"));
        free(error);
        if (wire) json_decref(wire);
        return response;
    }
    free(error);
    return success_response(id, wire);
}

/*
 * Every version between the floor and the one this SDK speaks, for the same
 * reason the host accepts a range: the host opens at the floor and raises as
 * it learns, so a provider that only recognised the two endpoints would refuse
 * whatever sits between them.
 */
static int supported_host_protocol(const json_t *protocol) {
    return maelys_mcp_json_string_equals(protocol, MAELYS_MCP_PROVIDER_PROTOCOL) ||
        maelys_mcp_json_string_equals(protocol, "maelys-provider/4") ||
        maelys_mcp_json_string_equals(protocol, MAELYS_MCP_PROVIDER_PROTOCOL_FLOOR);
}

static json_t *handle_message(maelys_mcp_provider_sdk_t *sdk, json_t *message) {
    json_t *id = json_is_object(message) ? json_object_get(message, "id") : NULL;
    json_t *protocol = json_is_object(message) ? json_object_get(message, "protocol") : NULL;
    json_t *method = json_is_object(message) ? json_object_get(message, "method") : NULL;
    json_t *params = json_is_object(message) ? json_object_get(message, "params") : NULL;
    if (!json_is_integer(id) ||
        !supported_host_protocol(protocol) ||
        !json_is_string(method) || maelys_mcp_json_string_has_nul(method) ||
        (params && !json_is_object(params))) {
        return error_response(json_is_integer(id) ? id : NULL,
            "provider_error", "invalid provider request");
    }
    const char *name = json_string_value(method);
    if (strcmp(name, "provider/describe") == 0) {
        json_t *description = describe_provider(sdk->config);
        return description ? success_response(id, description) :
            error_response(id, "provider_error", "cannot describe provider");
    }
    if (strcmp(name, "provider/activate") == 0) {
        char *error = NULL;
        maelys_mcp_result_t status = sdk->config->activate ?
            sdk->config->activate(sdk, sdk->config->context, &error) : MAELYS_MCP_OK;
        if (status != MAELYS_MCP_OK) {
            json_t *response = error_response(id, "provider_error",
                error ? error : "provider activation failed");
            free(error);
            return response;
        }
        free(error);
        return success_response(id, json_object());
    }
    if (strcmp(name, "provider/call") == 0) return handle_call(sdk, id, params);
    if (strcmp(name, "provider/readResource") == 0) return handle_read_resource(sdk, id, params);
    if (strcmp(name, "provider/shutdown") == 0) {
        /* No event may appear after the shutdown response. */
        shutdown_provider(sdk);
        return success_response(id, json_object());
    }
    return error_response(id, "provider_error", "method not found");
}

maelys_mcp_result_t maelys_mcp_provider_sdk_report_progress(
    maelys_mcp_provider_sdk_t *sdk,
    double progress,
    double total,
    const char *message) {
    if (!sdk) return MAELYS_MCP_ERR_ARGUMENT;
    json_t *params = json_object();
    json_t *notification = json_object();
    if (!params || !notification) goto failed;
    if (json_object_set_new(params, "progress", json_real(progress)) != 0 ||
        (total >= 0.0 && json_object_set_new(params, "total", json_real(total)) != 0) ||
        (message && json_object_set_new(params, "message", json_string(message)) != 0)) {
        goto failed;
    }
    if (json_object_set_new(notification, "protocol",
            json_string(MAELYS_MCP_PROVIDER_PROTOCOL)) != 0 ||
        json_object_set_new(notification, "method",
            json_string("provider/notifications/progress")) != 0) {
        goto failed;
    }
    int attached = json_object_set_new(notification, "params", params) == 0;
    params = NULL;
    if (!attached) goto failed;
    maelys_mcp_result_t status = write_message(sdk, notification);
    json_decref(notification);
    return status;
failed:
    if (params) json_decref(params);
    if (notification) json_decref(notification);
    return MAELYS_MCP_ERR_MEMORY;
}

maelys_mcp_result_t maelys_mcp_provider_sdk_emit_event(
    maelys_mcp_provider_sdk_t *sdk,
    const maelys_mcp_provider_event_t *event) {
    if (!sdk || !event ||
        (event->kind != MAELYS_MCP_PROVIDER_EVENT_RESOURCE_UPDATED &&
         event->kind != MAELYS_MCP_PROVIDER_EVENT_RESOURCES_LIST_CHANGED &&
         event->kind != MAELYS_MCP_PROVIDER_EVENT_TOOLS_LIST_CHANGED) ||
        (event->kind == MAELYS_MCP_PROVIDER_EVENT_RESOURCE_UPDATED &&
         (!event->resource_uri || !*event->resource_uri)) ||
        (event->kind != MAELYS_MCP_PROVIDER_EVENT_RESOURCE_UPDATED && event->resource_uri)) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    pthread_mutex_lock(&sdk->state_mutex);
    if (!sdk->activated || sdk->shutting_down) {
        pthread_mutex_unlock(&sdk->state_mutex);
        return MAELYS_MCP_ERR_DENIED;
    }
    sdk->events_inflight++;
    pthread_mutex_unlock(&sdk->state_mutex);
    const char *method = NULL;
    json_t *params = json_object();
    if (!params) goto memory_error;
    if (event->kind == MAELYS_MCP_PROVIDER_EVENT_RESOURCE_UPDATED) {
        method = "provider/notifications/resources/updated";
        if (json_object_set_new(params, "uri", json_string(event->resource_uri)) != 0) {
            json_decref(params);
            goto memory_error;
        }
    } else if (event->kind == MAELYS_MCP_PROVIDER_EVENT_RESOURCES_LIST_CHANGED) {
        method = "provider/notifications/resources/list_changed";
    } else {
        method = "provider/notifications/tools/list_changed";
    }
    json_t *message = json_object();
    if (!message ||
        json_object_set_new(message, "protocol",
            json_string(MAELYS_MCP_PROVIDER_PROTOCOL)) != 0 ||
        json_object_set_new(message, "method", json_string(method)) != 0 ||
        json_object_set_new(message, "params", params) != 0) {
        if (message) json_decref(message);
        else json_decref(params);
        goto memory_error;
    }
    maelys_mcp_result_t status = write_message(sdk, message);
    json_decref(message);
    pthread_mutex_lock(&sdk->state_mutex);
    sdk->events_inflight--;
    if (sdk->events_inflight == 0u) pthread_cond_broadcast(&sdk->events_idle);
    pthread_mutex_unlock(&sdk->state_mutex);
    return status;
memory_error:
    pthread_mutex_lock(&sdk->state_mutex);
    sdk->events_inflight--;
    if (sdk->events_inflight == 0u) pthread_cond_broadcast(&sdk->events_idle);
    pthread_mutex_unlock(&sdk->state_mutex);
    return MAELYS_MCP_ERR_MEMORY;
}

/*
 * The host's own nested_error_code (src/provider/process_provider.c) run in
 * reverse: that function is the host turning a result of the client round
 * trip into the wire string this SDK now has to read back. "client_error" is
 * folded into MAELYS_MCP_ERR_PROVIDER on purpose, mirroring the in-process
 * maelys_mcp_provider_request_client contract, where a JSON-RPC error from
 * the client is reported the same way - the error object itself travels in
 * `*out_result` rather than in the status code either way.
 */
static maelys_mcp_result_t nested_error_status(const json_t *code) {
    if (maelys_mcp_json_string_equals(code, "denied")) return MAELYS_MCP_ERR_DENIED;
    if (maelys_mcp_json_string_equals(code, "timeout")) return MAELYS_MCP_ERR_TIMEOUT;
    if (maelys_mcp_json_string_equals(code, "cancelled")) return MAELYS_MCP_ERR_CLOSED;
    if (maelys_mcp_json_string_equals(code, "unavailable")) return MAELYS_MCP_ERR_STATE;
    return MAELYS_MCP_ERR_PROVIDER;
}

/*
 * json_object_set_new consumes its value whether it succeeds or fails, so
 * every handover below stops owning the value on the same line it is handed
 * over - the same discipline nested_reply_message
 * (src/provider/process_provider.c) uses for the frame going the other way.
 */
static json_t *nested_request_message(const char *nested_id, const char *method, json_t *params) {
    json_t *inner = json_object();
    json_t *message = json_object();
    if (!inner || !message) goto failed;
    if (json_object_set_new(inner, "nestedId", json_string(nested_id)) != 0 ||
        json_object_set_new(inner, "method", json_string(method)) != 0 ||
        (params && json_object_set(inner, "params", params) != 0)) {
        goto failed;
    }
    if (json_object_set_new(message, "protocol",
            json_string(MAELYS_MCP_PROVIDER_PROTOCOL)) != 0 ||
        json_object_set_new(message, "method",
            json_string("provider/nestedRequest")) != 0) {
        goto failed;
    }
    int attached = json_object_set_new(message, "params", inner) == 0;
    inner = NULL;
    if (!attached) goto failed;
    return message;
failed:
    if (inner) json_decref(inner);
    if (message) json_decref(message);
    return NULL;
}

maelys_mcp_result_t maelys_mcp_provider_sdk_request_client(
    maelys_mcp_provider_sdk_t *sdk,
    const char *method,
    json_t *params,
    json_t **out_result,
    char **out_error) {
    if (!sdk || !method || !*method || !out_result) return MAELYS_MCP_ERR_ARGUMENT;
    *out_result = NULL;
    if (!sdk->reader || sdk->input_fd < 0) {
        replace_error(out_error, "the SDK is not serving a connection to nest a request on");
        return MAELYS_MCP_ERR_STATE;
    }
    pthread_mutex_lock(&sdk->state_mutex);
    if (sdk->nested_inflight) {
        pthread_mutex_unlock(&sdk->state_mutex);
        replace_error(out_error, "a nested client request is already outstanding");
        return MAELYS_MCP_ERR_STATE;
    }
    sdk->nested_inflight = 1;
    unsigned long long sequence = ++sdk->nested_sequence;
    pthread_mutex_unlock(&sdk->state_mutex);

    char nested_id[32];
    (void)snprintf(nested_id, sizeof(nested_id), "sdk-%llu", sequence);
    json_t *request = nested_request_message(nested_id, method, params);
    maelys_mcp_result_t status;
    if (!request) {
        replace_error(out_error, "cannot build the nested request");
        status = MAELYS_MCP_ERR_MEMORY;
        goto done;
    }
    status = write_message(sdk, request);
    json_decref(request);
    if (status != MAELYS_MCP_OK) {
        replace_error(out_error, "cannot write the nested request to the host");
        goto done;
    }

    /*
     * The wire is strictly single-outstanding: the host does not touch this
     * connection's exchange for any other purpose while it owes this reply,
     * so the next line read here is guaranteed - by the protocol, not by this
     * loop - to be the correlated provider/nestedReply. Anything else is the
     * host breaking that contract, reported as a malformed reply rather than
     * retried or ignored.
     */
    for (;;) {
        json_t *message = NULL;
        char *read_error = NULL;
        maelys_mcp_result_t read_status = maelys_mcp_line_reader_read(
            sdk->reader, sdk->input_fd, &message, &read_error);
        if (read_status == MAELYS_MCP_ERR_NOT_FOUND) {
            free(read_error);
            replace_error(out_error,
                "the host closed the connection while a nested request was outstanding");
            status = MAELYS_MCP_ERR_IO;
            break;
        }
        if (read_status != MAELYS_MCP_OK) {
            replace_error(out_error, read_error ? read_error :
                "cannot read the nested reply from the host");
            free(read_error);
            status = MAELYS_MCP_ERR_PROTOCOL;
            break;
        }
        free(read_error);
        json_t *reply_params = json_is_object(message) ?
            json_object_get(message, "params") : NULL;
        int shaped = json_is_object(message) &&
            maelys_mcp_json_string_equals(
                json_object_get(message, "method"), "provider/nestedReply") &&
            json_is_object(reply_params);
        json_t *reply_id = shaped ? json_object_get(reply_params, "nestedId") : NULL;
        shaped = shaped && json_is_string(reply_id) &&
            strcmp(json_string_value(reply_id), nested_id) == 0;
        json_t *result = shaped ? json_object_get(reply_params, "result") : NULL;
        json_t *error = shaped ? json_object_get(reply_params, "error") : NULL;
        shaped = shaped && (!!result != !!error) && (!error || json_is_object(error));
        if (!shaped) {
            json_decref(message);
            replace_error(out_error, "the host sent a malformed nested reply");
            status = MAELYS_MCP_ERR_PROTOCOL;
            break;
        }
        if (result) {
            *out_result = json_deep_copy(result);
            json_decref(message);
            status = *out_result ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
            if (status != MAELYS_MCP_OK) {
                replace_error(out_error, "cannot copy the client's nested result");
            }
            break;
        }
        json_t *code = json_object_get(error, "code");
        json_t *error_message = json_object_get(error, "message");
        json_t *data = json_object_get(error, "data");
        status = nested_error_status(code);
        replace_error(out_error, json_is_string(error_message) &&
            !maelys_mcp_json_string_has_nul(error_message) ?
            json_string_value(error_message) : "nested request failed");
        /* The client's own JSON-RPC error, carried on "client_error" so a
         * provider can tell "the user declined" from "the request never got
         * there" - mirroring maelys_mcp_provider_request_client. */
        if (data) *out_result = json_deep_copy(data);
        json_decref(message);
        break;
    }
done:
    pthread_mutex_lock(&sdk->state_mutex);
    sdk->nested_inflight = 0;
    pthread_mutex_unlock(&sdk->state_mutex);
    return status;
}

maelys_mcp_result_t maelys_mcp_provider_sdk_serve(
    const maelys_mcp_provider_sdk_config_t *config,
    const maelys_mcp_provider_sdk_options_t *options) {
    if (!provider_config_valid(config)) return MAELYS_MCP_ERR_ARGUMENT;
    int input_fd = options && options->input_fd > 0 ? options->input_fd : STDIN_FILENO;
    int output_fd = options && options->output_fd > 0 ? options->output_fd : STDOUT_FILENO;
    int close_output = 0;
    if ((!options || !options->disable_stdout_isolation) &&
        (!options || options->output_fd <= 0)) {
        int isolated = -1;
        maelys_mcp_result_t isolated_status = maelys_mcp_isolate_stdout(&isolated);
        if (isolated_status != MAELYS_MCP_OK) return isolated_status;
        output_fd = isolated;
        close_output = 1;
    }
    maelys_mcp_provider_sdk_t sdk = {
        .config = config,
        .output_fd = output_fd
    };
    if (pthread_mutex_init(&sdk.write_mutex, NULL) != 0) {
        if (close_output) close(output_fd);
        return MAELYS_MCP_ERR_IO;
    }
    sdk.write_mutex_initialized = 1;
    if (pthread_mutex_init(&sdk.state_mutex, NULL) != 0) {
        pthread_mutex_destroy(&sdk.write_mutex);
        if (close_output) close(output_fd);
        return MAELYS_MCP_ERR_IO;
    }
    sdk.state_mutex_initialized = 1;
    if (pthread_cond_init(&sdk.events_idle, NULL) != 0) {
        pthread_mutex_destroy(&sdk.state_mutex);
        pthread_mutex_destroy(&sdk.write_mutex);
        if (close_output) close(output_fd);
        return MAELYS_MCP_ERR_IO;
    }
    sdk.events_idle_initialized = 1;
    maelys_mcp_line_reader_t reader;
    maelys_mcp_result_t status = maelys_mcp_line_reader_init(
        &reader, options && options->max_message_bytes ?
            options->max_message_bytes : MAELYS_MCP_DEFAULT_MAX_MESSAGE_BYTES);
    if (status != MAELYS_MCP_OK) goto done;
    /* Published only once the reader is live, so a callback invoked before
     * this point (there is none today, but nothing stops one being added)
     * sees a NULL reader rather than a half-initialized one. */
    sdk.reader = &reader;
    sdk.input_fd = input_fd;
    for (;;) {
        pthread_mutex_lock(&sdk.state_mutex);
        int shutting_down = sdk.shutting_down;
        pthread_mutex_unlock(&sdk.state_mutex);
        if (shutting_down) break;
        json_t *message = NULL;
        char *error = NULL;
        status = maelys_mcp_line_reader_read(&reader, input_fd, &message, &error);
        if (status == MAELYS_MCP_ERR_NOT_FOUND) {
            free(error);
            status = MAELYS_MCP_OK;
            break;
        }
        if (status != MAELYS_MCP_OK) {
            json_t *response = error_response(NULL, "provider_error", error);
            free(error);
            if (!response) {
                status = MAELYS_MCP_ERR_MEMORY;
                break;
            }
            status = write_message(&sdk, response);
            json_decref(response);
            if (status != MAELYS_MCP_OK) break;
            continue;
        }
        int was_activate = json_is_object(message) &&
            maelys_mcp_json_string_equals(json_object_get(message, "method"), "provider/activate");
        json_t *response = handle_message(&sdk, message);
        json_decref(message);
        if (!response) {
            status = MAELYS_MCP_ERR_MEMORY;
            break;
        }
        int activate = was_activate && json_is_object(json_object_get(response, "result"));
        status = write_message(&sdk, response);
        json_decref(response);
        if (status != MAELYS_MCP_OK) break;
        if (activate) {
            pthread_mutex_lock(&sdk.state_mutex);
            if (!sdk.shutting_down) sdk.activated = 1;
            pthread_mutex_unlock(&sdk.state_mutex);
        }
    }
    maelys_mcp_line_reader_clear(&reader);
done:
    shutdown_provider(&sdk);
    if (config->destroy) config->destroy(config->context);
    if (sdk.events_idle_initialized) pthread_cond_destroy(&sdk.events_idle);
    if (sdk.state_mutex_initialized) pthread_mutex_destroy(&sdk.state_mutex);
    if (sdk.write_mutex_initialized) pthread_mutex_destroy(&sdk.write_mutex);
    if (close_output) close(output_fd);
    return status;
}
