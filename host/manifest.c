#include "host/manifest.h"

#include "src/internal/internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Strict, three-stage validation (docs/manifest.md):
 *
 *   1. JSON syntax (jansson's own parser, with its line/column info).
 *   2. Semantic validation over the whole document - types, required keys,
 *      unknown keys, value ranges - producing one precise error naming the
 *      offending key and its location ("providers[1].path", "allowEffects[0]",
 *      ...) and nothing else.
 *   3. Construction, which assumes stage 2 already passed and can only fail
 *      on allocation.
 *
 * Stage 3 never starts until stage 2 has walked the *entire* document
 * successfully: a manifest that is wrong in provider 3 must not leave
 * providers 0-2 half-built in *out_manifest.
 */

static void manifest_errorf(char **out_error, const char *format, ...) {
    if (!out_error) return;
    va_list args;
    va_start(args, format);
    char buffer[512];
    (void)vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    free(*out_error);
    *out_error = maelys_mcp_strdup(buffer);
}

/* A sane upper bound for maxMessageBytes - generous (16 GiB) but finite, so a
 * bogus huge value is rejected here rather than turning into an oversized
 * calloc deep inside a spawn call. */
#define MANIFEST_MAX_BYTES ((json_int_t)(16LL * 1024 * 1024 * 1024))

static const char *const TOP_LEVEL_KEYS[] = {
    "manifestVersion", "providers", "allowEffects", NULL
};
static const char *const NATIVE_KEYS[] = {
    "type", "path", "describeTimeoutMs", "callTimeoutMs", "shutdownTimeoutMs",
    "maxMessageBytes", NULL
};
static const char *const PROXY_KEYS[] = {
    "type", "path", "argv", "toolPrefix", "defaultEffect", "schemaPolicy",
    "connectTimeoutMs", "callTimeoutMs", "maxMessageBytes", NULL
};

static int key_known(const char *key, const char *const *allowed) {
    for (; *allowed; ++allowed) {
        if (strcmp(key, *allowed) == 0) return 1;
    }
    return 0;
}

static maelys_mcp_result_t reject_unknown_keys(
    json_t *object,
    const char *const *allowed,
    const char *location,
    char **out_error) {
    const char *key;
    json_t *value;
    json_object_foreach(object, key, value) {
        if (!key_known(key, allowed)) {
            manifest_errorf(out_error, "unknown key \"%s\" in %s", key, location);
            return MAELYS_MCP_ERR_ARGUMENT;
        }
    }
    return MAELYS_MCP_OK;
}

/* json_t strings are validated for embedded NULs everywhere a C string will
 * be made from them (json_string_value truncates silently otherwise). */
static int clean_string(json_t *value) {
    return json_is_string(value) && !maelys_mcp_json_string_has_nul(value);
}

static maelys_mcp_result_t validate_optional_uint(
    json_t *object,
    const char *field,
    json_int_t max_value,
    const char *location,
    char **out_error) {
    json_t *value = json_object_get(object, field);
    if (!value) return MAELYS_MCP_OK;
    if (!json_is_integer(value) || json_integer_value(value) < 0 ||
        json_integer_value(value) > max_value) {
        manifest_errorf(out_error, "%s.%s must be a non-negative integer", location, field);
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    return MAELYS_MCP_OK;
}

/* `location` is the whole path to `value` (e.g. "providers[1].defaultEffect"
 * or "allowEffects[0]"), not just a field name. */
static maelys_mcp_result_t validate_effect_string(
    json_t *value,
    const char *location,
    int allow_gated_only,
    char **out_error) {
    if (!clean_string(value)) {
        manifest_errorf(out_error, "%s must be a string", location);
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    maelys_mcp_tool_effect_t effect = MAELYS_MCP_EFFECT_UNSPECIFIED;
    if (maelys_mcp_tool_effect_parse(json_string_value(value), &effect) != MAELYS_MCP_OK) {
        manifest_errorf(out_error, "%s is not a known effect class: \"%s\"",
            location, json_string_value(value));
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    if (allow_gated_only &&
        (effect == MAELYS_MCP_EFFECT_READ || effect == MAELYS_MCP_EFFECT_PREVIEW)) {
        manifest_errorf(out_error,
            "%s must be one of \"apply\", \"commit\", \"execute\", found \"%s\"",
            location, json_string_value(value));
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t validate_provider(
    json_t *provider,
    const char *location,
    char **out_error) {
    if (!json_is_object(provider)) {
        manifest_errorf(out_error, "%s must be an object", location);
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    json_t *type = json_object_get(provider, "type");
    if (!type) {
        manifest_errorf(out_error, "%s is missing required key \"type\"", location);
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    if (!clean_string(type)) {
        manifest_errorf(out_error, "%s.type must be a string", location);
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    int is_native = strcmp(json_string_value(type), "native") == 0;
    int is_proxy = strcmp(json_string_value(type), "mcp-proxy") == 0;
    if (!is_native && !is_proxy) {
        manifest_errorf(out_error,
            "%s.type must be \"native\" or \"mcp-proxy\", found \"%s\"",
            location, json_string_value(type));
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    maelys_mcp_result_t status = reject_unknown_keys(
        provider, is_native ? NATIVE_KEYS : PROXY_KEYS, location, out_error);
    if (status != MAELYS_MCP_OK) return status;

    json_t *path = json_object_get(provider, "path");
    if (!path) {
        manifest_errorf(out_error, "%s is missing required key \"path\"", location);
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    if (!clean_string(path) || json_string_length(path) == 0u) {
        manifest_errorf(out_error, "%s.path must be a non-empty string", location);
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    if (json_string_value(path)[0] != '/') {
        manifest_errorf(out_error, "%s.path must be an absolute path, found \"%s\"",
            location, json_string_value(path));
        return MAELYS_MCP_ERR_ARGUMENT;
    }

    if (is_native) {
        if ((status = validate_optional_uint(provider, "describeTimeoutMs",
            UINT_MAX, location, out_error)) != MAELYS_MCP_OK) return status;
        if ((status = validate_optional_uint(provider, "callTimeoutMs",
            UINT_MAX, location, out_error)) != MAELYS_MCP_OK) return status;
        if ((status = validate_optional_uint(provider, "shutdownTimeoutMs",
            UINT_MAX, location, out_error)) != MAELYS_MCP_OK) return status;
        if ((status = validate_optional_uint(provider, "maxMessageBytes",
            MANIFEST_MAX_BYTES, location, out_error)) != MAELYS_MCP_OK) return status;
        return MAELYS_MCP_OK;
    }

    if ((status = validate_optional_uint(provider, "connectTimeoutMs",
        UINT_MAX, location, out_error)) != MAELYS_MCP_OK) return status;
    if ((status = validate_optional_uint(provider, "callTimeoutMs",
        UINT_MAX, location, out_error)) != MAELYS_MCP_OK) return status;
    if ((status = validate_optional_uint(provider, "maxMessageBytes",
        MANIFEST_MAX_BYTES, location, out_error)) != MAELYS_MCP_OK) return status;

    json_t *argv = json_object_get(provider, "argv");
    if (argv) {
        if (!json_is_array(argv)) {
            manifest_errorf(out_error, "%s.argv must be an array of strings", location);
            return MAELYS_MCP_ERR_ARGUMENT;
        }
        size_t count = json_array_size(argv);
        for (size_t index = 0; index < count; ++index) {
            if (!clean_string(json_array_get(argv, index))) {
                manifest_errorf(out_error, "%s.argv[%zu] must be a string", location, index);
                return MAELYS_MCP_ERR_ARGUMENT;
            }
        }
    }
    json_t *prefix = json_object_get(provider, "toolPrefix");
    if (prefix && (!clean_string(prefix) || json_string_length(prefix) == 0u)) {
        manifest_errorf(out_error, "%s.toolPrefix must be a non-empty string", location);
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    json_t *default_effect = json_object_get(provider, "defaultEffect");
    if (default_effect) {
        char effect_location[80];
        (void)snprintf(effect_location, sizeof(effect_location),
            "%s.defaultEffect", location);
        status = validate_effect_string(default_effect, effect_location, 0, out_error);
        if (status != MAELYS_MCP_OK) return status;
    }
    json_t *schema_policy = json_object_get(provider, "schemaPolicy");
    if (schema_policy) {
        if (!clean_string(schema_policy)) {
            manifest_errorf(out_error, "%s.schemaPolicy must be a string", location);
            return MAELYS_MCP_ERR_ARGUMENT;
        }
        const char *value = json_string_value(schema_policy);
        if (strcmp(value, "strict") != 0 && strcmp(value, "skip") != 0 &&
            strcmp(value, "passthrough") != 0) {
            manifest_errorf(out_error,
                "%s.schemaPolicy must be one of \"strict\", \"skip\", \"passthrough\", found \"%s\"",
                location, value);
            return MAELYS_MCP_ERR_ARGUMENT;
        }
    }
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t validate_manifest(json_t *root, char **out_error) {
    if (!json_is_object(root)) {
        manifest_errorf(out_error, "manifest root must be a JSON object");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    maelys_mcp_result_t status = reject_unknown_keys(root, TOP_LEVEL_KEYS, "manifest", out_error);
    if (status != MAELYS_MCP_OK) return status;

    json_t *version = json_object_get(root, "manifestVersion");
    if (!version) {
        manifest_errorf(out_error, "manifest is missing required key \"manifestVersion\"");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    if (!json_is_integer(version) || json_integer_value(version) != 1) {
        manifest_errorf(out_error, "manifestVersion must be 1");
        return MAELYS_MCP_ERR_ARGUMENT;
    }

    json_t *providers = json_object_get(root, "providers");
    if (!providers) {
        manifest_errorf(out_error, "manifest is missing required key \"providers\"");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    if (!json_is_array(providers) || json_array_size(providers) == 0u) {
        manifest_errorf(out_error, "providers must be a non-empty array");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    size_t count = json_array_size(providers);
    for (size_t index = 0; index < count; ++index) {
        char location[64];
        (void)snprintf(location, sizeof(location), "providers[%zu]", index);
        status = validate_provider(json_array_get(providers, index), location, out_error);
        if (status != MAELYS_MCP_OK) return status;
    }

    json_t *allow_effects = json_object_get(root, "allowEffects");
    if (allow_effects) {
        if (!json_is_array(allow_effects)) {
            manifest_errorf(out_error, "allowEffects must be an array");
            return MAELYS_MCP_ERR_ARGUMENT;
        }
        size_t effect_count = json_array_size(allow_effects);
        for (size_t index = 0; index < effect_count; ++index) {
            char location[32];
            (void)snprintf(location, sizeof(location), "allowEffects[%zu]", index);
            status = validate_effect_string(
                json_array_get(allow_effects, index), location, 1, out_error);
            if (status != MAELYS_MCP_OK) return status;
        }
    }
    return MAELYS_MCP_OK;
}

static unsigned int uint_field(json_t *object, const char *field) {
    json_t *value = json_object_get(object, field);
    return value ? (unsigned int)json_integer_value(value) : 0u;
}

static size_t size_field(json_t *object, const char *field) {
    json_t *value = json_object_get(object, field);
    return value ? (size_t)json_integer_value(value) : 0u;
}

/* Assumes validate_manifest already accepted the whole document: only an
 * allocation failure can turn this back. */
static maelys_mcp_result_t build_provider(json_t *provider, manifest_provider_t *target) {
    const char *type_value = json_string_value(json_object_get(provider, "type"));
    target->type = strcmp(type_value, "native") == 0 ?
        MANIFEST_PROVIDER_NATIVE : MANIFEST_PROVIDER_MCP_PROXY;
    target->path = maelys_mcp_strdup(
        json_string_value(json_object_get(provider, "path")));
    if (!target->path) return MAELYS_MCP_ERR_MEMORY;
    target->call_timeout_ms = uint_field(provider, "callTimeoutMs");
    target->max_message_bytes = size_field(provider, "maxMessageBytes");

    if (target->type == MANIFEST_PROVIDER_NATIVE) {
        target->describe_timeout_ms = uint_field(provider, "describeTimeoutMs");
        target->shutdown_timeout_ms = uint_field(provider, "shutdownTimeoutMs");
        return MAELYS_MCP_OK;
    }

    target->connect_timeout_ms = uint_field(provider, "connectTimeoutMs");
    json_t *argv = json_object_get(provider, "argv");
    if (argv) {
        size_t count = json_array_size(argv);
        target->argv = calloc(count + 1u, sizeof(*target->argv));
        if (!target->argv) return MAELYS_MCP_ERR_MEMORY;
        for (size_t index = 0; index < count; ++index) {
            target->argv[index] = maelys_mcp_strdup(
                json_string_value(json_array_get(argv, index)));
            if (!target->argv[index]) return MAELYS_MCP_ERR_MEMORY;
        }
        target->argv[count] = NULL;
        target->argv_count = count;
    }
    json_t *prefix = json_object_get(provider, "toolPrefix");
    if (prefix) {
        target->tool_prefix = maelys_mcp_strdup(json_string_value(prefix));
        if (!target->tool_prefix) return MAELYS_MCP_ERR_MEMORY;
    }
    json_t *default_effect = json_object_get(provider, "defaultEffect");
    target->default_effect = MAELYS_MCP_EFFECT_UNSPECIFIED;
    if (default_effect) {
        /* Already validated: cannot fail. */
        (void)maelys_mcp_tool_effect_parse(
            json_string_value(default_effect), &target->default_effect);
    }
    json_t *schema_policy = json_object_get(provider, "schemaPolicy");
    target->schema_policy = MAELYS_MCP_PROXY_SCHEMA_STRICT;
    if (schema_policy) {
        const char *value = json_string_value(schema_policy);
        target->schema_policy = strcmp(value, "skip") == 0 ?
            MAELYS_MCP_PROXY_SCHEMA_SKIP : strcmp(value, "passthrough") == 0 ?
            MAELYS_MCP_PROXY_SCHEMA_PASSTHROUGH : MAELYS_MCP_PROXY_SCHEMA_STRICT;
    }
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t build_manifest(json_t *root, manifest_t *out) {
    out->version = (int)json_integer_value(json_object_get(root, "manifestVersion"));
    json_t *providers = json_object_get(root, "providers");
    size_t count = json_array_size(providers);
    out->providers = calloc(count, sizeof(*out->providers));
    if (!out->providers) return MAELYS_MCP_ERR_MEMORY;
    for (size_t index = 0; index < count; ++index) {
        maelys_mcp_result_t status = build_provider(
            json_array_get(providers, index), &out->providers[index]);
        /* Every entry up to and including this one must be freeable by
         * manifest_clear even when this loop stops early. */
        out->provider_count = index + 1u;
        if (status != MAELYS_MCP_OK) return status;
    }
    out->provider_count = count;

    out->allowed_effects = 0u;
    json_t *allow_effects = json_object_get(root, "allowEffects");
    if (allow_effects) {
        size_t effect_count = json_array_size(allow_effects);
        for (size_t index = 0; index < effect_count; ++index) {
            maelys_mcp_tool_effect_t effect = MAELYS_MCP_EFFECT_UNSPECIFIED;
            /* Already validated: cannot fail. */
            (void)maelys_mcp_tool_effect_parse(
                json_string_value(json_array_get(allow_effects, index)), &effect);
            out->allowed_effects |= 1u << (unsigned int)effect;
        }
    }
    return MAELYS_MCP_OK;
}

maelys_mcp_result_t manifest_load(
    const char *path,
    manifest_t *out_manifest,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (!path || !out_manifest) return MAELYS_MCP_ERR_ARGUMENT;
    memset(out_manifest, 0, sizeof(*out_manifest));

    json_error_t json_error;
    json_t *root = json_load_file(path, 0, &json_error);
    if (!root) {
        manifest_errorf(out_error, "manifest JSON syntax error in %s at line %d column %d: %s",
            json_error.source, json_error.line, json_error.column, json_error.text);
        return MAELYS_MCP_ERR_ARGUMENT;
    }

    maelys_mcp_result_t status = validate_manifest(root, out_error);
    if (status != MAELYS_MCP_OK) {
        json_decref(root);
        return status;
    }
    /* Stage 3 only starts now that the whole document has validated. */
    status = build_manifest(root, out_manifest);
    json_decref(root);
    if (status != MAELYS_MCP_OK) {
        manifest_clear(out_manifest);
        manifest_errorf(out_error, "cannot allocate the parsed manifest");
        return status;
    }
    return MAELYS_MCP_OK;
}

void manifest_clear(manifest_t *manifest) {
    if (!manifest) return;
    for (size_t index = 0; index < manifest->provider_count; ++index) {
        manifest_provider_t *provider = &manifest->providers[index];
        free(provider->path);
        free(provider->tool_prefix);
        for (size_t arg = 0; arg < provider->argv_count; ++arg) {
            free(provider->argv[arg]);
        }
        free(provider->argv);
    }
    free(manifest->providers);
    memset(manifest, 0, sizeof(*manifest));
}
