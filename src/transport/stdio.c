#include "src/internal/internal.h"

#include <stdlib.h>

maelys_mcp_result_t maelys_mcp_runtime_serve_stdio(
    maelys_mcp_runtime_t *runtime,
    int read_fd,
    int write_fd) {
    if (!runtime || read_fd < 0 || write_fd < 0) return MAELYS_MCP_ERR_ARGUMENT;
    for (;;) {
        json_t *request = NULL;
        char *error = NULL;
        maelys_mcp_result_t status = maelys_mcp_read_json_line(
            read_fd, runtime->max_message_bytes, &request, &error);
        if (status == MAELYS_MCP_ERR_NOT_FOUND) {
            free(error);
            return MAELYS_MCP_OK;
        }
        if (status != MAELYS_MCP_OK) {
            json_t *response = maelys_mcp_error_response(NULL, -32700,
                error ? error : "Parse error", NULL);
            free(error);
            if (!response) return MAELYS_MCP_ERR_MEMORY;
            status = maelys_mcp_write_json_line(write_fd, response);
            json_decref(response);
            if (status != MAELYS_MCP_OK) return status;
            continue;
        }
        json_t *response = maelys_mcp_runtime_handle(runtime, request);
        json_decref(request);
        if (!response) continue;
        status = maelys_mcp_write_json_line(write_fd, response);
        json_decref(response);
        if (status != MAELYS_MCP_OK) return status;
    }
}
