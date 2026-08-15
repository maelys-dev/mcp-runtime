#include "maelys/mcp/provider_sdk.h"

#include <jansson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct example_context {
    json_t *echo_input;
    json_t *echo_output;
    json_t *sum_input;
    json_t *events_input;
} example_context_t;

static json_t *tool_schema_message(void) {
    return json_pack("{s:s,s:{s:{s:s,s:i,s:i}},s:[s],s:b}",
        "type", "object",
        "properties", "message", "type", "string", "minLength", 1, "maxLength", 256,
        "required", "message",
        "additionalProperties", 0);
}

static json_t *tool_schema_sum(void) {
    return json_pack("{s:s,s:{s:{s:s},s:{s:s}},s:[s,s],s:b}",
        "type", "object",
        "properties",
            "left", "type", "number",
            "right", "type", "number",
        "required", "left", "right",
        "additionalProperties", 0);
}

static json_t *output_schema_message(void) {
    return json_pack("{s:s,s:{s:{s:s},s:{s:s}},s:[s,s],s:b}",
        "type", "object",
        "properties",
            "message", "type", "string",
            "provider", "type", "string",
        "required", "message", "provider",
        "additionalProperties", 0);
}

static void set_error(char **out_error, const char *message) {
    if (!out_error) return;
    char *copy = strdup(message ? message : "example provider error");
    if (!copy) return;
    free(*out_error);
    *out_error = copy;
}

static maelys_mcp_result_t call_tool(
    maelys_mcp_provider_sdk_t *sdk,
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    (void)context;
    if (strcmp(request->tool_name, "example.echo") == 0) {
        json_t *message = json_object_get(request->arguments, "message");
        if (!json_is_string(message)) {
            set_error(out_error, "message is required");
            return MAELYS_MCP_ERR_ARGUMENT;
        }
        out_result->type = MAELYS_MCP_PROVIDER_RESULT_COMPLETE;
        out_result->structured_content = json_pack("{s:s,s:s}",
            "message", json_string_value(message),
            "provider", "example");
        return out_result->structured_content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
    }
    if (strcmp(request->tool_name, "example.sum") == 0) {
        json_t *left = json_object_get(request->arguments, "left");
        json_t *right = json_object_get(request->arguments, "right");
        if (!json_is_number(left) || !json_is_number(right)) {
            set_error(out_error, "left and right are required");
            return MAELYS_MCP_ERR_ARGUMENT;
        }
        out_result->type = MAELYS_MCP_PROVIDER_RESULT_COMPLETE;
        out_result->structured_content = json_pack("{s:f}",
            "value", json_number_value(left) + json_number_value(right));
        return out_result->structured_content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
    }
    if (strcmp(request->tool_name, "example.events") == 0) {
        const maelys_mcp_provider_event_t updated = {
            .kind = MAELYS_MCP_PROVIDER_EVENT_RESOURCE_UPDATED,
            .resource_uri = "example://about"
        };
        const maelys_mcp_provider_event_t resources = {
            .kind = MAELYS_MCP_PROVIDER_EVENT_RESOURCES_LIST_CHANGED
        };
        const maelys_mcp_provider_event_t tools = {
            .kind = MAELYS_MCP_PROVIDER_EVENT_TOOLS_LIST_CHANGED
        };
        if (maelys_mcp_provider_sdk_emit_event(sdk, &updated) != MAELYS_MCP_OK ||
            maelys_mcp_provider_sdk_emit_event(sdk, &resources) != MAELYS_MCP_OK ||
            maelys_mcp_provider_sdk_emit_event(sdk, &tools) != MAELYS_MCP_OK) {
            set_error(out_error, "cannot emit provider events");
            return MAELYS_MCP_ERR_IO;
        }
        out_result->type = MAELYS_MCP_PROVIDER_RESULT_COMPLETE;
        out_result->structured_content = json_pack("{s:i}", "emitted", 3);
        return out_result->structured_content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
    }
    set_error(out_error, "unknown tool");
    return MAELYS_MCP_ERR_NOT_FOUND;
}

static maelys_mcp_result_t read_resource(
    maelys_mcp_provider_sdk_t *sdk,
    void *context,
    const maelys_mcp_resource_request_t *request,
    maelys_mcp_resource_result_t *out_result,
    char **out_error) {
    (void)sdk;
    (void)context;
    if (strcmp(request->uri, "example://about") != 0 &&
        strncmp(request->uri, "example://echo/", strlen("example://echo/")) != 0) {
        set_error(out_error, "resource not found");
        return MAELYS_MCP_ERR_NOT_FOUND;
    }
    const char *text = strcmp(request->uri, "example://about") == 0 ?
        "Example resource provider" : request->uri + strlen("example://echo/");
    out_result->type = MAELYS_MCP_RESOURCE_RESULT_COMPLETE;
    out_result->contents = json_pack("[{s:s,s:s,s:s}]",
        "uri", request->uri, "mimeType", "text/plain", "text", text);
    return out_result->contents ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static void destroy_context(void *context) {
    example_context_t *example = context;
    if (!example) return;
    if (example->echo_input) json_decref(example->echo_input);
    if (example->echo_output) json_decref(example->echo_output);
    if (example->sum_input) json_decref(example->sum_input);
    if (example->events_input) json_decref(example->events_input);
}

int main(void) {
    example_context_t context = {
        .echo_input = tool_schema_message(),
        .echo_output = output_schema_message(),
        .sum_input = tool_schema_sum(),
        .events_input = json_pack("{s:s}", "type", "object")
    };
    if (!context.echo_input || !context.echo_output || !context.sum_input || !context.events_input) {
        destroy_context(&context);
        return 1;
    }
    const maelys_mcp_tool_t tools[] = {
        {
            .name = "example.echo",
            .title = "Echo a message",
            .description = "Returns the supplied message through an external provider.",
            .input_schema = context.echo_input,
            .output_schema = context.echo_output,
            .effect = MAELYS_MCP_EFFECT_READ
        },
        {
            .name = "example.sum",
            .title = "Add two numbers",
            .description = "Adds two numbers in an external provider.",
            .input_schema = context.sum_input,
            .effect = MAELYS_MCP_EFFECT_READ
        },
        {
            .name = "example.events",
            .title = "Emit provider events",
            .description = "Emits resource and catalog change events before completing.",
            .input_schema = context.events_input,
            .effect = MAELYS_MCP_EFFECT_EXECUTE
        }
    };
    const maelys_mcp_resource_t resources[] = {
        {
            .uri = "example://about",
            .name = "About example provider",
            .mime_type = "text/plain",
            .has_size = 1,
            .size = 24
        }
    };
    const maelys_mcp_resource_template_t templates[] = {
        {
            .uri_template = "example://echo/{value}",
            .name = "Echo resource",
            .mime_type = "text/plain"
        }
    };
    const maelys_mcp_provider_sdk_config_t config = {
        .name = "example",
        .version = "0.9.0",
        .tools = tools,
        .tool_count = sizeof(tools) / sizeof(tools[0]),
        .resources = resources,
        .resource_count = sizeof(resources) / sizeof(resources[0]),
        .resource_templates = templates,
        .resource_template_count = sizeof(templates) / sizeof(templates[0]),
        .call = call_tool,
        .read_resource = read_resource,
        .destroy = destroy_context,
        .context = &context
    };
    maelys_mcp_result_t status = maelys_mcp_provider_sdk_serve(&config, NULL);
    return status == MAELYS_MCP_OK ? 0 : 1;
}
