#include "maelys/mcp/uri.h"
#include "src/internal/internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <uriparser/Uri.h>

#define MAELYS_URI_DEFAULT_MAX_BYTES 8192u

struct maelys_uri {
    char *canonical;
    size_t canonical_length;
    char *scheme;
};

static void uri_error(char **out_error, const char *message) {
    if (!out_error) return;
    free(*out_error);
    *out_error = maelys_mcp_strdup(message);
}

static int encoded_nul(const char *bytes, size_t length) {
    for (size_t index = 0; index + 2u < length; ++index) {
        if (bytes[index] == '%' && bytes[index + 1u] == '0' && bytes[index + 2u] == '0') {
            return 1;
        }
    }
    return 0;
}

maelys_mcp_result_t maelys_uri_parse(
    const char *bytes,
    size_t length,
    const maelys_uri_options_t *options,
    maelys_uri_t **out_uri,
    char **out_error) {
    if (!bytes || !length || !out_uri) return MAELYS_MCP_ERR_ARGUMENT;
    *out_uri = NULL;
    size_t max_bytes = options && options->max_bytes ?
        options->max_bytes : MAELYS_URI_DEFAULT_MAX_BYTES;
    if (length > max_bytes || memchr(bytes, '\0', length) || encoded_nul(bytes, length)) {
        uri_error(out_error, length > max_bytes ?
            "URI exceeds configured limit" : "URI contains a forbidden NUL encoding");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    UriUriA parsed;
    memset(&parsed, 0, sizeof(parsed));
    const char *error_position = NULL;
    if (uriParseSingleUriExA(&parsed, bytes, bytes + length, &error_position) != URI_SUCCESS) {
        uriFreeUriMembersA(&parsed);
        uri_error(out_error, "URI is not valid RFC 3986 syntax");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    if (options && options->require_scheme && !parsed.scheme.first) {
        uriFreeUriMembersA(&parsed);
        uri_error(out_error, "URI requires a scheme");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    if (uriNormalizeSyntaxA(&parsed) != URI_SUCCESS) {
        uriFreeUriMembersA(&parsed);
        uri_error(out_error, "URI normalization failed");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    int required = 0;
    if (uriToStringCharsRequiredA(&parsed, &required) != URI_SUCCESS || required < 0 ||
        (size_t)required > max_bytes) {
        uriFreeUriMembersA(&parsed);
        uri_error(out_error, "normalized URI exceeds configured limit");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    maelys_uri_t *uri = calloc(1u, sizeof(*uri));
    char *canonical = malloc((size_t)required + 1u);
    if (!uri || !canonical) {
        free(uri);
        free(canonical);
        uriFreeUriMembersA(&parsed);
        return MAELYS_MCP_ERR_MEMORY;
    }
    int written = 0;
    if (uriToStringA(canonical, &parsed, required + 1, &written) != URI_SUCCESS ||
        written != required + 1) {
        free(canonical);
        free(uri);
        uriFreeUriMembersA(&parsed);
        uri_error(out_error, "URI serialization failed");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    uriFreeUriMembersA(&parsed);
    size_t scheme_length = 0;
    while (scheme_length < (size_t)required && canonical[scheme_length] != ':') {
        ++scheme_length;
    }
    if (scheme_length < (size_t)required) {
        uri->scheme = malloc(scheme_length + 1u);
        if (!uri->scheme) {
            free(canonical);
            free(uri);
            return MAELYS_MCP_ERR_MEMORY;
        }
        memcpy(uri->scheme, canonical, scheme_length);
        uri->scheme[scheme_length] = '\0';
    }
    uri->canonical = canonical;
    uri->canonical_length = (size_t)required;
    *out_uri = uri;
    return MAELYS_MCP_OK;
}

const char *maelys_uri_canonical(const maelys_uri_t *uri) {
    return uri ? uri->canonical : NULL;
}

size_t maelys_uri_canonical_length(const maelys_uri_t *uri) {
    return uri ? uri->canonical_length : 0u;
}

const char *maelys_uri_scheme(const maelys_uri_t *uri) {
    return uri ? uri->scheme : NULL;
}

int maelys_uri_has_scheme(const maelys_uri_t *uri, const char *scheme) {
    return uri && uri->scheme && scheme && strcmp(uri->scheme, scheme) == 0;
}

void maelys_uri_destroy(maelys_uri_t *uri) {
    if (!uri) return;
    free(uri->canonical);
    free(uri->scheme);
    free(uri);
}

static int valid_variable(const char *bytes, size_t length) {
    if (!length) return 0;
    size_t modifier = length;
    for (size_t index = 0; index < length; ++index) {
        if (bytes[index] == ':' || bytes[index] == '*') {
            modifier = index;
            break;
        }
        unsigned char byte = (unsigned char)bytes[index];
        if (!(isalnum(byte) || byte == '_' || byte == '.' || byte == '%')) return 0;
        if (byte == '%' && (index + 2u >= length || !isxdigit((unsigned char)bytes[index + 1u]) ||
            !isxdigit((unsigned char)bytes[index + 2u]))) return 0;
        if (byte == '%') index += 2u;
    }
    if (!modifier) return 0;
    if (modifier == length) return 1;
    if (bytes[modifier] == '*') return modifier + 1u == length;
    if (modifier + 1u == length) return 0;
    for (size_t index = modifier + 1u; index < length; ++index) {
        if (!isdigit((unsigned char)bytes[index])) return 0;
    }
    return 1;
}

static int valid_expression(const char *bytes, size_t length) {
    if (!length) return 0;
    if (strchr("+#./;?&", bytes[0])) {
        ++bytes;
        --length;
    }
    if (!length) return 0;
    size_t start = 0;
    for (size_t index = 0; index <= length; ++index) {
        if (index == length || bytes[index] == ',') {
            if (!valid_variable(bytes + start, index - start)) return 0;
            start = index + 1u;
        }
    }
    return 1;
}

maelys_mcp_result_t maelys_uri_template_validate(
    const char *bytes,
    size_t length,
    size_t max_bytes,
    char **out_error) {
    if (!bytes || !length) return MAELYS_MCP_ERR_ARGUMENT;
    if (!max_bytes) max_bytes = MAELYS_URI_DEFAULT_MAX_BYTES;
    if (length > max_bytes || memchr(bytes, '\0', length) || encoded_nul(bytes, length)) {
        uri_error(out_error, "URI template exceeds limits or contains NUL");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    char *surrogate = malloc(length * 4u + 1u);
    if (!surrogate) return MAELYS_MCP_ERR_MEMORY;
    size_t output = 0;
    for (size_t index = 0; index < length;) {
        if (bytes[index] == '}') goto invalid;
        if (bytes[index] != '{') {
            surrogate[output++] = bytes[index++];
            continue;
        }
        size_t end = index + 1u;
        while (end < length && bytes[end] != '}') {
            if (bytes[end] == '{') goto invalid;
            ++end;
        }
        if (end == length || !valid_expression(bytes + index + 1u, end - index - 1u)) {
            goto invalid;
        }
        char operator = bytes[index + 1u];
        const char *replacement = "x";
        if (operator == '?') replacement = "?x=x";
        else if (operator == '&') replacement = "&x=x";
        else if (operator == '#') replacement = "#x";
        else if (operator == '/') replacement = "/x";
        else if (operator == '.') replacement = ".x";
        else if (operator == ';') replacement = ";x";
        size_t replacement_length = strlen(replacement);
        memcpy(surrogate + output, replacement, replacement_length);
        output += replacement_length;
        index = end + 1u;
    }
    surrogate[output] = '\0';
    maelys_uri_t *parsed = NULL;
    maelys_uri_options_t options = {.max_bytes = max_bytes, .require_scheme = 1};
    maelys_mcp_result_t status = maelys_uri_parse(
        surrogate, output, &options, &parsed, out_error);
    maelys_uri_destroy(parsed);
    free(surrogate);
    return status;
invalid:
    free(surrogate);
    uri_error(out_error, "URI template is not valid RFC 6570 syntax");
    return MAELYS_MCP_ERR_PROTOCOL;
}
