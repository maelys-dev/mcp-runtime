#pragma once

#include <stddef.h>
#include <jansson.h>

#include "maelys/mcp/error.h"

#ifdef __cplusplus
extern "C" {
#endif

maelys_mcp_result_t maelys_mcp_validate_content(
    json_t *content,
    size_t max_encoded_bytes,
    char **out_error);

#ifdef __cplusplus
}
#endif
