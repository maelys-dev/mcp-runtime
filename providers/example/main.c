#include <jansson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static json_t *description(void) {
    json_t *tools = json_array();
    json_t *echo = json_object();
    json_t *sum = json_object();
    json_t *echo_input = tool_schema_message();
    json_t *echo_output = output_schema_message();
    json_t *sum_input = tool_schema_sum();
    if (!tools || !echo || !sum || !echo_input || !echo_output || !sum_input) return NULL;
    json_object_set_new(echo, "name", json_string("example.echo"));
    json_object_set_new(echo, "title", json_string("Echo a message"));
    json_object_set_new(echo, "description", json_string("Returns the supplied message through an external provider."));
    json_object_set_new(echo, "inputSchema", echo_input);
    json_object_set_new(echo, "outputSchema", echo_output);
    json_object_set_new(echo, "effect", json_string("read"));
    json_array_append_new(tools, echo);
    json_object_set_new(sum, "name", json_string("example.sum"));
    json_object_set_new(sum, "title", json_string("Add two numbers"));
    json_object_set_new(sum, "description", json_string("Adds two numbers in an external provider."));
    json_object_set_new(sum, "inputSchema", sum_input);
    json_object_set_new(sum, "effect", json_string("read"));
    json_array_append_new(tools, sum);
    return json_pack("{s:s,s:s,s:o}", "name", "example", "version", "0.1.0", "tools", tools);
}

static json_t *call_tool(json_t *params, const char **out_error) {
    json_t *name = json_object_get(params, "name");
    json_t *arguments = json_object_get(params, "arguments");
    if (!json_is_string(name) || !json_is_object(arguments)) {
        *out_error = "invalid provider call";
        return NULL;
    }
    if (strcmp(json_string_value(name), "example.echo") == 0) {
        json_t *message = json_object_get(arguments, "message");
        if (!json_is_string(message)) { *out_error = "message is required"; return NULL; }
        return json_pack("{s:s,s:{s:s,s:s}}",
            "resultType", "complete", "structuredContent",
            "message", json_string_value(message), "provider", "example");
    }
    if (strcmp(json_string_value(name), "example.sum") == 0) {
        json_t *left = json_object_get(arguments, "left");
        json_t *right = json_object_get(arguments, "right");
        if (!json_is_number(left) || !json_is_number(right)) { *out_error = "left and right are required"; return NULL; }
        return json_pack("{s:s,s:{s:f}}", "resultType", "complete",
            "structuredContent", "value", json_number_value(left) + json_number_value(right));
    }
    *out_error = "unknown tool";
    return NULL;
}

int main(void) {
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, stdin) >= 0) {
        json_error_t parse_error;
        json_t *request = json_loads(line, JSON_REJECT_DUPLICATES, &parse_error);
        if (!json_is_object(request)) { if (request) json_decref(request); continue; }
        json_t *id = json_object_get(request, "id");
        json_t *method = json_object_get(request, "method");
        json_t *params = json_object_get(request, "params");
        json_t *response = json_pack("{s:s,s:O}", "protocol", "maelys-provider/2", "id", id);
        int should_exit = 0;
        if (json_is_string(method) && strcmp(json_string_value(method), "provider/describe") == 0) {
            json_object_set_new(response, "result", description());
        } else if (json_is_string(method) && strcmp(json_string_value(method), "provider/call") == 0) {
            const char *message = NULL;
            json_t *result = call_tool(params, &message);
            if (result) json_object_set_new(response, "result", result);
            else json_object_set_new(response, "error", json_pack("{s:i,s:s}", "code", 1, "message", message));
        } else if (json_is_string(method) && strcmp(json_string_value(method), "provider/shutdown") == 0) {
            json_object_set_new(response, "result", json_object());
            should_exit = 1;
        } else {
            json_object_set_new(response, "error", json_pack("{s:i,s:s}", "code", 2, "message", "method not found"));
        }
        json_dumpf(response, stdout, JSON_COMPACT | JSON_SORT_KEYS);
        fputc('\n', stdout);
        fflush(stdout);
        json_decref(response);
        json_decref(request);
        if (should_exit) break;
    }
    free(line);
    return 0;
}
