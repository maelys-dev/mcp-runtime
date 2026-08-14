#pragma once

#include <stddef.h>

#include "maelys/mcp/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_uri maelys_uri_t;

typedef struct maelys_uri_options {
    size_t max_bytes;
    int require_scheme;
} maelys_uri_options_t;

maelys_mcp_result_t maelys_uri_parse(
    const char *bytes,
    size_t length,
    const maelys_uri_options_t *options,
    maelys_uri_t **out_uri,
    char **out_error);

const char *maelys_uri_canonical(const maelys_uri_t *uri);
size_t maelys_uri_canonical_length(const maelys_uri_t *uri);
const char *maelys_uri_scheme(const maelys_uri_t *uri);
int maelys_uri_has_scheme(const maelys_uri_t *uri, const char *scheme);
void maelys_uri_destroy(maelys_uri_t *uri);

maelys_mcp_result_t maelys_uri_template_validate(
    const char *bytes,
    size_t length,
    size_t max_bytes,
    char **out_error);

#ifdef __cplusplus
}
#endif
