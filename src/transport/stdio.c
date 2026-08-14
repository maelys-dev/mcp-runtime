#include "src/internal/internal.h"
#include "maelys/mcp/outbox.h"

#include <stdint.h>
#include <stdlib.h>

static maelys_mcp_result_t write_stdio_message(void *context, json_t *message) {
    int write_fd = *(const int *)context;
    return maelys_mcp_write_json_line(write_fd, message);
}

maelys_mcp_result_t maelys_mcp_runtime_serve_stdio(
    maelys_mcp_runtime_t *runtime,
    int read_fd,
    int write_fd) {
    if (!runtime || read_fd < 0 || write_fd < 0) return MAELYS_MCP_ERR_ARGUMENT;
    maelys_mcp_line_reader_t reader;
    maelys_mcp_result_t initialized = maelys_mcp_line_reader_init(
        &reader, runtime->max_message_bytes);
    if (initialized != MAELYS_MCP_OK) return initialized;
    maelys_mcp_outbox_config_t outbox_config = {
        .max_messages = 256u,
        .max_bytes = runtime->max_message_bytes > SIZE_MAX / 4u ?
            SIZE_MAX : runtime->max_message_bytes * 4u,
        .batch_size = 32u,
        .response_burst = 8u,
        .write = write_stdio_message,
        .write_context = &write_fd
    };
    maelys_mcp_outbox_t *outbox = NULL;
    initialized = maelys_mcp_outbox_create(&outbox_config, &outbox);
    if (initialized != MAELYS_MCP_OK) {
        maelys_mcp_line_reader_clear(&reader);
        return initialized;
    }
    for (;;) {
        json_t *request = NULL;
        char *error = NULL;
        maelys_mcp_result_t status = maelys_mcp_line_reader_read(
            &reader, read_fd, &request, &error);
        if (status == MAELYS_MCP_ERR_NOT_FOUND) {
            free(error);
            maelys_mcp_line_reader_clear(&reader);
            return maelys_mcp_outbox_destroy(outbox, 1);
        }
        if (status != MAELYS_MCP_OK) {
            json_t *response = maelys_mcp_error_response(NULL, -32700,
                error ? error : "Parse error", NULL);
            free(error);
            if (!response) {
                maelys_mcp_line_reader_clear(&reader);
                (void)maelys_mcp_outbox_destroy(outbox, 0);
                return MAELYS_MCP_ERR_MEMORY;
            }
            status = maelys_mcp_outbox_enqueue_take(
                outbox, response, MAELYS_MCP_OUTBOX_RESPONSE, NULL);
            if (status != MAELYS_MCP_OK) {
                json_decref(response);
                maelys_mcp_line_reader_clear(&reader);
                (void)maelys_mcp_outbox_destroy(outbox, 0);
                return status;
            }
            continue;
        }
        json_t *response = maelys_mcp_runtime_handle(runtime, request);
        json_decref(request);
        if (!response) continue;
        status = maelys_mcp_outbox_enqueue_take(
            outbox, response, MAELYS_MCP_OUTBOX_RESPONSE, NULL);
        if (status != MAELYS_MCP_OK) {
            json_decref(response);
            maelys_mcp_line_reader_clear(&reader);
            (void)maelys_mcp_outbox_destroy(outbox, 0);
            return status;
        }
    }
}
