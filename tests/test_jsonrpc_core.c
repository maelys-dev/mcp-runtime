#include "src/jsonrpc/core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_TRUE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Assertion failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct recorder {
    int count;
    json_t *last;
} recorder_t;

static maelys_mcp_result_t record(void *context, json_t *message) {
    recorder_t *recorder = context;
    recorder->count++;
    if (recorder->last) json_decref(recorder->last);
    recorder->last = message;
    return MAELYS_MCP_OK;
}

int main(void) {
    recorder_t recorder = {0};
    maelys_mcp_jsonrpc_core_t core;
    maelys_mcp_jsonrpc_options_t options = {
        .framing = MAELYS_MCP_JSONRPC_JSON_LINES,
        .max_line_bytes = 64,
        .on_message = record,
        .context = &recorder
    };
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(&core, &options) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_feed(&core, "{\"a\":", 5) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_has_buffered_bytes(&core));
    const char *line_tail = "1}\n{\"b\":2}\r\n";
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_feed(&core, line_tail, strlen(line_tail)) == MAELYS_MCP_OK);
    ASSERT_TRUE(recorder.count == 2);
    ASSERT_TRUE(json_integer_value(json_object_get(recorder.last, "b")) == 2);

    json_t *message = json_pack("{s:s}", "jsonrpc", "2.0");
    char *frame = NULL;
    size_t frame_length = 0;
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_serialize(&core, message, &frame, &frame_length) == MAELYS_MCP_OK);
    ASSERT_TRUE(frame_length > 1 && frame[frame_length - 1] == '\n');
    free(frame);
    json_decref(message);
    maelys_mcp_jsonrpc_core_clear(&core);
    json_decref(recorder.last);

    memset(&recorder, 0, sizeof(recorder));
    options.framing = MAELYS_MCP_JSONRPC_CONTENT_LENGTH;
    options.max_body_bytes = 64;
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(&core, &options) == MAELYS_MCP_OK);
    const char *content_frame = "Content-Length: 7\r\n\r\n{\"x\":1}";
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_feed(&core, content_frame, 12) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_feed(&core, content_frame + 12,
        strlen(content_frame) - 12) == MAELYS_MCP_OK);
    ASSERT_TRUE(recorder.count == 1);
    ASSERT_TRUE(json_integer_value(json_object_get(recorder.last, "x")) == 1);
    maelys_mcp_jsonrpc_core_clear(&core);
    json_decref(recorder.last);

    memset(&recorder, 0, sizeof(recorder));
    options.framing = MAELYS_MCP_JSONRPC_JSON_LINES;
    options.max_line_bytes = 8;
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(&core, &options) == MAELYS_MCP_OK);
    const char *oversized = "{\"duplicate\":1}\n";
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_feed(&core, oversized, strlen(oversized)) == MAELYS_MCP_ERR_PROTOCOL);
    maelys_mcp_jsonrpc_core_clear(&core);

    puts("test_jsonrpc_core: OK");
    return 0;
}
