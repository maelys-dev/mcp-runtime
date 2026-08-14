#include "src/jsonrpc/core.h"
#include "tests/test_support.h"

#include <stdlib.h>
#include <string.h>

typedef struct recorder {
    int count;
    json_t *last;
    maelys_mcp_result_t result;
} recorder_t;

static maelys_mcp_result_t record(void *context, json_t *message) {
    recorder_t *recorder = context;
    recorder->count++;
    if (recorder->last) json_decref(recorder->last);
    recorder->last = message;
    return recorder->result;
}

static maelys_mcp_jsonrpc_options_t options(
    recorder_t *recorder,
    maelys_mcp_jsonrpc_framing_t framing) {
    maelys_mcp_jsonrpc_options_t value = {
        .framing = framing,
        .max_header_bytes = 128,
        .max_body_bytes = 128,
        .max_line_bytes = 128,
        .on_message = record,
        .context = recorder
    };
    return value;
}

static void clear(maelys_mcp_jsonrpc_core_t *core, recorder_t *recorder) {
    maelys_mcp_jsonrpc_core_clear(core);
    if (recorder->last) json_decref(recorder->last);
    recorder->last = NULL;
}

static int test_init_contract(void) {
    maelys_mcp_jsonrpc_core_t core;
    recorder_t recorder = {0};
    maelys_mcp_jsonrpc_options_t valid = options(&recorder, MAELYS_MCP_JSONRPC_JSON_LINES);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(NULL, &valid) == MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(&core, NULL) == MAELYS_MCP_ERR_ARGUMENT);
    valid.on_message = NULL;
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(&core, &valid) == MAELYS_MCP_ERR_ARGUMENT);
    valid.on_message = record;
    valid.framing = (maelys_mcp_jsonrpc_framing_t)99;
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(&core, &valid) == MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_finish(NULL) == MAELYS_MCP_ERR_ARGUMENT);
    return 0;
}

static int test_lines_fragmented_multiple_and_crlf(void) {
    maelys_mcp_jsonrpc_core_t core;
    recorder_t recorder = {0};
    maelys_mcp_jsonrpc_options_t value = options(&recorder, MAELYS_MCP_JSONRPC_JSON_LINES);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(&core, &value) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_feed(&core, "{\"a\":", 5) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_has_buffered_bytes(&core));
    const char *tail = "1}\n{\"b\":2}\r\n";
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_feed(&core, tail, strlen(tail)) == MAELYS_MCP_OK);
    ASSERT_TRUE(recorder.count == 2);
    ASSERT_TRUE(json_integer_value(json_object_get(recorder.last, "b")) == 2);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_finish(&core) == MAELYS_MCP_OK);
    clear(&core, &recorder);
    return 0;
}

static int test_lines_protocol_errors(void) {
    static const char *invalid[] = {
        "\n", "not-json\n", "{\"id\":1,\"id\":2}\n"
    };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        maelys_mcp_jsonrpc_core_t core;
        recorder_t recorder = {0};
        maelys_mcp_jsonrpc_options_t value = options(&recorder, MAELYS_MCP_JSONRPC_JSON_LINES);
        ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(&core, &value) == MAELYS_MCP_OK);
        ASSERT_TRUE(maelys_mcp_jsonrpc_core_feed(&core, invalid[index], strlen(invalid[index])) == MAELYS_MCP_ERR_PROTOCOL);
        ASSERT_TRUE(recorder.count == 0);
        clear(&core, &recorder);
    }
    return 0;
}

static int test_lines_limits_and_finish(void) {
    maelys_mcp_jsonrpc_core_t core;
    recorder_t recorder = {0};
    maelys_mcp_jsonrpc_options_t value = options(&recorder, MAELYS_MCP_JSONRPC_JSON_LINES);
    value.max_line_bytes = 7;
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(&core, &value) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_feed(&core, "{\"x\":1}\n", strlen("{\"x\":1}\n")) == MAELYS_MCP_OK);
    ASSERT_TRUE(recorder.count == 1);
    clear(&core, &recorder);

    memset(&recorder, 0, sizeof(recorder));
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(&core, &value) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_feed(&core, "12345678", 8) == MAELYS_MCP_ERR_PROTOCOL);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_finish(&core) == MAELYS_MCP_ERR_PROTOCOL);
    clear(&core, &recorder);
    return 0;
}

static int test_callback_error_is_propagated(void) {
    maelys_mcp_jsonrpc_core_t core;
    recorder_t recorder = {.result = MAELYS_MCP_ERR_IO};
    maelys_mcp_jsonrpc_options_t value = options(&recorder, MAELYS_MCP_JSONRPC_JSON_LINES);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(&core, &value) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_feed(&core, "{}\n", 3) == MAELYS_MCP_ERR_IO);
    ASSERT_TRUE(recorder.count == 1);
    clear(&core, &recorder);
    return 0;
}

static int test_content_length_fragmented_and_multiple(void) {
    maelys_mcp_jsonrpc_core_t core;
    recorder_t recorder = {0};
    maelys_mcp_jsonrpc_options_t value = options(&recorder, MAELYS_MCP_JSONRPC_CONTENT_LENGTH);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(&core, &value) == MAELYS_MCP_OK);
    const char *frames = "content-length: 7\r\nX-Test: yes\r\n\r\n{\"a\":1}Content-Length: 7\r\n\r\n{\"b\":2}";
    for (size_t index = 0; index < strlen(frames); ++index) {
        ASSERT_TRUE(maelys_mcp_jsonrpc_core_feed(&core, frames + index, 1) == MAELYS_MCP_OK);
    }
    ASSERT_TRUE(recorder.count == 2);
    ASSERT_TRUE(json_integer_value(json_object_get(recorder.last, "b")) == 2);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_finish(&core) == MAELYS_MCP_OK);
    clear(&core, &recorder);
    return 0;
}

static int expect_frame_error(const char *frame) {
    maelys_mcp_jsonrpc_core_t core;
    recorder_t recorder = {0};
    maelys_mcp_jsonrpc_options_t value = options(&recorder, MAELYS_MCP_JSONRPC_CONTENT_LENGTH);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(&core, &value) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_feed(&core, frame, strlen(frame)) == MAELYS_MCP_ERR_PROTOCOL);
    ASSERT_TRUE(recorder.count == 0);
    clear(&core, &recorder);
    return 0;
}

static int test_content_length_header_errors(void) {
    static const char *invalid[] = {
        "X: 1\r\n\r\n{}",
        "Content-Length:\r\n\r\n{}",
        "Content-Length: x\r\n\r\n{}",
        "Content-Length: -1\r\n\r\n{}",
        "Content-Length: 2x\r\n\r\n{}",
        "Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}",
        "Content-Length: 999999999999999999999999999999999\r\n\r\n{}"
    };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        ASSERT_TRUE(expect_frame_error(invalid[index]) == 0);
    }
    return 0;
}

static int test_content_length_body_errors(void) {
    ASSERT_TRUE(expect_frame_error("Content-Length: 0\r\n\r\n") == 0);
    ASSERT_TRUE(expect_frame_error("Content-Length: 7\r\n\r\nnotjson") == 0);
    ASSERT_TRUE(expect_frame_error("Content-Length: 13\r\n\r\n{\"x\":1,\"x\":2}") == 0);

    maelys_mcp_jsonrpc_core_t core;
    recorder_t recorder = {0};
    maelys_mcp_jsonrpc_options_t value = options(&recorder, MAELYS_MCP_JSONRPC_CONTENT_LENGTH);
    value.max_body_bytes = 1;
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(&core, &value) == MAELYS_MCP_OK);
    const char *oversized = "Content-Length: 2\r\n\r\n{}";
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_feed(&core, oversized, strlen(oversized)) == MAELYS_MCP_ERR_PROTOCOL);
    clear(&core, &recorder);
    return 0;
}

static int test_content_length_limits_and_finish(void) {
    maelys_mcp_jsonrpc_core_t core;
    recorder_t recorder = {0};
    maelys_mcp_jsonrpc_options_t value = options(&recorder, MAELYS_MCP_JSONRPC_CONTENT_LENGTH);
    value.max_header_bytes = 8;
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(&core, &value) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_feed(&core, "123456789", 9) == MAELYS_MCP_ERR_PROTOCOL);
    clear(&core, &recorder);

    memset(&recorder, 0, sizeof(recorder));
    value.max_header_bytes = 128;
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(&core, &value) == MAELYS_MCP_OK);
    const char *partial = "Content-Length: 7\r\n\r\n{\"x\":";
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_feed(&core, partial, strlen(partial)) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_finish(&core) == MAELYS_MCP_ERR_PROTOCOL);
    clear(&core, &recorder);
    return 0;
}

static int test_serialize_both_framings(void) {
    recorder_t recorder = {0};
    maelys_mcp_jsonrpc_core_t core;
    maelys_mcp_jsonrpc_options_t value = options(&recorder, MAELYS_MCP_JSONRPC_JSON_LINES);
    json_t *message = json_pack("{s:i,s:s}", "b", 2, "a", "x");
    ASSERT_TRUE(message != NULL);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(&core, &value) == MAELYS_MCP_OK);
    char *frame = NULL;
    size_t length = 0;
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_serialize(&core, message, &frame, &length) == MAELYS_MCP_OK);
    static const char line_expected[] = "{\"a\":\"x\",\"b\":2}\n";
    ASSERT_TRUE(length == sizeof(line_expected) - 1u);
    ASSERT_TRUE(memcmp(frame, line_expected, length) == 0);
    free(frame);
    clear(&core, &recorder);

    value.framing = MAELYS_MCP_JSONRPC_CONTENT_LENGTH;
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(&core, &value) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_serialize(&core, message, &frame, &length) == MAELYS_MCP_OK);
    static const char content_expected[] = "Content-Length: 15\r\n\r\n{\"a\":\"x\",\"b\":2}";
    ASSERT_TRUE(length == sizeof(content_expected) - 1u);
    ASSERT_TRUE(memcmp(frame, content_expected, length) == 0);
    free(frame);
    clear(&core, &recorder);
    json_decref(message);
    return 0;
}

static int test_feed_and_serialize_arguments(void) {
    recorder_t recorder = {0};
    maelys_mcp_jsonrpc_core_t core;
    maelys_mcp_jsonrpc_options_t value = options(&recorder, MAELYS_MCP_JSONRPC_JSON_LINES);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_init(&core, &value) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_feed(NULL, NULL, 0) == MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_feed(&core, NULL, 1) == MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_feed(&core, NULL, 0) == MAELYS_MCP_OK);
    json_t *message = json_object();
    char *bytes = NULL;
    size_t length = 0;
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_serialize(NULL, message, &bytes, &length) == MAELYS_MCP_ERR_ARGUMENT);
    core.framing = (maelys_mcp_jsonrpc_framing_t)99;
    ASSERT_TRUE(maelys_mcp_jsonrpc_core_serialize(&core, message, &bytes, &length) == MAELYS_MCP_ERR_ARGUMENT);
    json_decref(message);
    clear(&core, &recorder);
    return 0;
}

int main(void) {
    static const maelys_test_case_t tests[] = {
        {"init contract", test_init_contract},
        {"JSON Lines fragmented, multiple, CRLF", test_lines_fragmented_multiple_and_crlf},
        {"JSON Lines protocol errors and duplicate keys", test_lines_protocol_errors},
        {"JSON Lines limits and EOF", test_lines_limits_and_finish},
        {"callback error propagation and ownership", test_callback_error_is_propagated},
        {"Content-Length byte fragmentation and multiple frames", test_content_length_fragmented_and_multiple},
        {"Content-Length malformed headers", test_content_length_header_errors},
        {"Content-Length malformed and oversized bodies", test_content_length_body_errors},
        {"Content-Length limits and EOF", test_content_length_limits_and_finish},
        {"serialization for both framings", test_serialize_both_framings},
        {"feed and serialize argument validation", test_feed_and_serialize_arguments}
    };
    return maelys_run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
