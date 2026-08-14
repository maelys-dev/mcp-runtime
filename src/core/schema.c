#include "src/internal/internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void schema_error(char **out_error, const char *format, ...) {
    if (!out_error) return;
    va_list args;
    va_start(args, format);
    char buffer[512];
    (void)vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    free(*out_error);
    *out_error = maelys_mcp_strdup(buffer);
}

static int type_matches(const char *type, json_t *value) {
    if (strcmp(type, "object") == 0) return json_is_object(value);
    if (strcmp(type, "array") == 0) return json_is_array(value);
    if (strcmp(type, "string") == 0) return json_is_string(value);
    if (strcmp(type, "integer") == 0) return json_is_integer(value);
    if (strcmp(type, "number") == 0) return json_is_number(value);
    if (strcmp(type, "boolean") == 0) return json_is_boolean(value);
    if (strcmp(type, "null") == 0) return json_is_null(value);
    return 0;
}

static maelys_mcp_result_t validate_value(json_t *schema, json_t *value, const char *path, char **out_error);

static maelys_mcp_result_t validate_object(json_t *schema, json_t *value, const char *path, char **out_error) {
    json_t *required = json_object_get(schema, "required");
    if (required && !json_is_array(required)) {
        schema_error(out_error, "%s: schema.required must be an array", path);
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    size_t index;
    json_t *entry;
    json_array_foreach(required, index, entry) {
        if (!json_is_string(entry)) continue;
        const char *name = json_string_value(entry);
        if (!json_object_get(value, name)) {
            schema_error(out_error, "%s.%s is required", path, name);
            return MAELYS_MCP_ERR_ARGUMENT;
        }
    }

    json_t *properties = json_object_get(schema, "properties");
    json_t *additional = json_object_get(schema, "additionalProperties");
    const char *key;
    json_t *child;
    json_object_foreach(value, key, child) {
        json_t *child_schema = json_is_object(properties) ? json_object_get(properties, key) : NULL;
        if (!child_schema && json_is_false(additional)) {
            schema_error(out_error, "%s.%s is not allowed", path, key);
            return MAELYS_MCP_ERR_ARGUMENT;
        }
        if (child_schema) {
            char child_path[512];
            (void)snprintf(child_path, sizeof(child_path), "%s.%s", path, key);
            maelys_mcp_result_t result = validate_value(child_schema, child, child_path, out_error);
            if (result != MAELYS_MCP_OK) return result;
        }
    }
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t validate_value(json_t *schema, json_t *value, const char *path, char **out_error) {
    if (!json_is_object(schema)) {
        schema_error(out_error, "%s: schema must be an object", path);
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    json_t *type = json_object_get(schema, "type");
    if (json_is_string(type) && !type_matches(json_string_value(type), value)) {
        schema_error(out_error, "%s must have type %s", path, json_string_value(type));
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    json_t *enumeration = json_object_get(schema, "enum");
    if (json_is_array(enumeration)) {
        int found = 0;
        size_t index;
        json_t *candidate;
        json_array_foreach(enumeration, index, candidate) {
            if (json_equal(candidate, value)) { found = 1; break; }
        }
        if (!found) {
            schema_error(out_error, "%s is not an allowed value", path);
            return MAELYS_MCP_ERR_ARGUMENT;
        }
    }
    if (json_is_string(value)) {
        size_t length = json_string_length(value);
        json_t *minimum = json_object_get(schema, "minLength");
        json_t *maximum = json_object_get(schema, "maxLength");
        if (json_is_integer(minimum) && length < (size_t)json_integer_value(minimum)) {
            schema_error(out_error, "%s is shorter than minLength", path);
            return MAELYS_MCP_ERR_ARGUMENT;
        }
        if (json_is_integer(maximum) && length > (size_t)json_integer_value(maximum)) {
            schema_error(out_error, "%s exceeds maxLength", path);
            return MAELYS_MCP_ERR_ARGUMENT;
        }
    }
    if (json_is_number(value)) {
        double number = json_number_value(value);
        json_t *minimum = json_object_get(schema, "minimum");
        json_t *maximum = json_object_get(schema, "maximum");
        if (json_is_number(minimum) && number < json_number_value(minimum)) {
            schema_error(out_error, "%s is below minimum", path);
            return MAELYS_MCP_ERR_ARGUMENT;
        }
        if (json_is_number(maximum) && number > json_number_value(maximum)) {
            schema_error(out_error, "%s exceeds maximum", path);
            return MAELYS_MCP_ERR_ARGUMENT;
        }
    }
    if (json_is_object(value)) return validate_object(schema, value, path, out_error);
    if (json_is_array(value)) {
        json_t *items = json_object_get(schema, "items");
        if (items) {
            size_t index;
            json_t *child;
            json_array_foreach(value, index, child) {
                char child_path[512];
                (void)snprintf(child_path, sizeof(child_path), "%s[%zu]", path, index);
                maelys_mcp_result_t result = validate_value(items, child, child_path, out_error);
                if (result != MAELYS_MCP_OK) return result;
            }
        }
    }
    return MAELYS_MCP_OK;
}

maelys_mcp_result_t maelys_mcp_validate_schema(json_t *schema, json_t *value, char **out_error) {
    if (out_error) *out_error = NULL;
    if (!schema || !value) return MAELYS_MCP_ERR_ARGUMENT;
    return validate_value(schema, value, "$", out_error);
}
