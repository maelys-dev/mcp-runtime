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

static int known_type(const char *type) {
    return type && (
        strcmp(type, "object") == 0 || strcmp(type, "array") == 0 ||
        strcmp(type, "string") == 0 || strcmp(type, "integer") == 0 ||
        strcmp(type, "number") == 0 || strcmp(type, "boolean") == 0 ||
        strcmp(type, "null") == 0);
}

static int known_keyword(const char *key) {
    static const char *keywords[] = {
        "$schema", "title", "description", "type", "properties", "required",
        "additionalProperties", "items", "enum", "minLength", "maxLength",
        "minimum", "maximum"
    };
    for (size_t index = 0; index < sizeof(keywords) / sizeof(keywords[0]); ++index) {
        if (strcmp(key, keywords[index]) == 0) return 1;
    }
    return 0;
}

static maelys_mcp_result_t validate_definition(
    json_t *schema,
    const char *path,
    char **out_error) {
    if (!json_is_object(schema)) {
        schema_error(out_error, "%s: schema must be an object", path);
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    const char *key;
    json_t *entry;
    json_object_foreach(schema, key, entry) {
        if (!known_keyword(key)) {
            schema_error(out_error, "%s: unsupported schema keyword '%s'", path, key);
            return MAELYS_MCP_ERR_PROTOCOL;
        }
    }
    json_t *type_value = json_object_get(schema, "type");
    if (!json_is_string(type_value) || maelys_mcp_json_string_has_nul(type_value) ||
        !known_type(json_string_value(type_value))) {
        schema_error(out_error, "%s.type must name one supported JSON type", path);
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    const char *type = json_string_value(type_value);
    json_t *properties = json_object_get(schema, "properties");
    json_t *required = json_object_get(schema, "required");
    json_t *additional = json_object_get(schema, "additionalProperties");
    json_t *items = json_object_get(schema, "items");
    json_t *minimum_length = json_object_get(schema, "minLength");
    json_t *maximum_length = json_object_get(schema, "maxLength");
    json_t *minimum = json_object_get(schema, "minimum");
    json_t *maximum = json_object_get(schema, "maximum");

    if ((properties || required || additional) && strcmp(type, "object") != 0) {
        schema_error(out_error, "%s: object keywords require type object", path);
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    if (properties && !json_is_object(properties)) {
        schema_error(out_error, "%s.properties must be an object", path);
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    json_object_foreach(properties, key, entry) {
        char child_path[512];
        (void)snprintf(child_path, sizeof(child_path), "%s.properties.%s", path, key);
        maelys_mcp_result_t result = validate_definition(entry, child_path, out_error);
        if (result != MAELYS_MCP_OK) return result;
    }
    if (required) {
        if (!json_is_array(required)) {
            schema_error(out_error, "%s.required must be an array", path);
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        size_t index;
        json_t *name;
        json_array_foreach(required, index, name) {
            if (!json_is_string(name) || json_string_length(name) == 0u ||
                maelys_mcp_json_string_has_nul(name)) {
                schema_error(out_error, "%s.required[%zu] must be a non-empty string", path, index);
                return MAELYS_MCP_ERR_PROTOCOL;
            }
            for (size_t previous = 0; previous < index; ++previous) {
                if (strcmp(json_string_value(name),
                    json_string_value(json_array_get(required, previous))) == 0) {
                    schema_error(out_error, "%s.required contains duplicate '%s'", path,
                        json_string_value(name));
                    return MAELYS_MCP_ERR_PROTOCOL;
                }
            }
            if (!properties || !json_object_get(properties, json_string_value(name))) {
                schema_error(out_error, "%s.required names undeclared property '%s'", path,
                    json_string_value(name));
                return MAELYS_MCP_ERR_PROTOCOL;
            }
        }
    }
    if (additional && !json_is_boolean(additional)) {
        schema_error(out_error, "%s.additionalProperties must be boolean", path);
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    if (items) {
        if (strcmp(type, "array") != 0) {
            schema_error(out_error, "%s.items requires type array", path);
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        maelys_mcp_result_t result = validate_definition(items, path, out_error);
        if (result != MAELYS_MCP_OK) return result;
    }
    if ((minimum_length || maximum_length) && strcmp(type, "string") != 0) {
        schema_error(out_error, "%s: length keywords require type string", path);
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    if ((minimum_length && (!json_is_integer(minimum_length) || json_integer_value(minimum_length) < 0)) ||
        (maximum_length && (!json_is_integer(maximum_length) || json_integer_value(maximum_length) < 0)) ||
        (minimum_length && maximum_length &&
            json_integer_value(minimum_length) > json_integer_value(maximum_length))) {
        schema_error(out_error, "%s: invalid minLength/maxLength", path);
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    if ((minimum || maximum) && strcmp(type, "number") != 0 && strcmp(type, "integer") != 0) {
        schema_error(out_error, "%s: numeric bounds require type number or integer", path);
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    if ((minimum && !json_is_number(minimum)) || (maximum && !json_is_number(maximum)) ||
        (minimum && maximum && json_number_value(minimum) > json_number_value(maximum))) {
        schema_error(out_error, "%s: invalid minimum/maximum", path);
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    json_t *enumeration = json_object_get(schema, "enum");
    if (enumeration) {
        if (!json_is_array(enumeration) || json_array_size(enumeration) == 0) {
            schema_error(out_error, "%s.enum must be a non-empty array", path);
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        size_t index;
        json_t *candidate;
        json_array_foreach(enumeration, index, candidate) {
            if (!type_matches(type, candidate)) {
                schema_error(out_error, "%s.enum[%zu] does not match type %s", path, index, type);
                return MAELYS_MCP_ERR_PROTOCOL;
            }
        }
    }
    return MAELYS_MCP_OK;
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

maelys_mcp_result_t maelys_mcp_validate_schema_definition(
    json_t *schema,
    int require_object_root,
    char **out_error) {
    if (out_error) *out_error = NULL;
    maelys_mcp_result_t result = validate_definition(schema, "$", out_error);
    if (result != MAELYS_MCP_OK) return result;
    if (require_object_root &&
        strcmp(json_string_value(json_object_get(schema, "type")), "object") != 0) {
        schema_error(out_error, "$.type must be object for MCP tool arguments");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    return MAELYS_MCP_OK;
}
