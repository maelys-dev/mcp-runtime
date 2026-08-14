#include "maelys/mcp/uri.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || !size) return 0;
    maelys_uri_options_t options = {.max_bytes = 8192u, .require_scheme = 1};
    maelys_uri_t *uri = NULL;
    char *error = NULL;
    (void)maelys_uri_parse((const char *)data, size, &options, &uri, &error);
    maelys_uri_destroy(uri);
    free(error);
    error = NULL;
    (void)maelys_uri_template_validate(
        (const char *)data, size, 8192u, &error);
    free(error);
    return 0;
}
