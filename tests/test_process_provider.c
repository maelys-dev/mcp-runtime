#include "maelys/mcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ASSERT_TRUE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Assertion failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static json_t *next_message(maelys_mcp_channel_t *channel) {
    json_t *message = NULL;
    return maelys_mcp_channel_next(channel, 2000u, &message) == MAELYS_MCP_OK ?
        message : NULL;
}

static json_t *dispatch(maelys_mcp_channel_t *channel, json_t *request) {
    maelys_mcp_result_t status = maelys_mcp_channel_handle(channel, request);
    json_decref(request);
    return status == MAELYS_MCP_OK ? next_message(channel) : NULL;
}

static long long milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (long long)now.tv_sec * 1000LL + (long long)now.tv_nsec / 1000000LL;
}

/*
 * Spawn a fixture this suite expects to come up. The describe deadline is a
 * liveness bound the suite does not measure, and the default 5s has missed
 * under a sanitizer on a loaded machine - the reason tests/test_provider_perf.c
 * gives, and the same loose bound tests/test_nested_requests.c uses. Every
 * other timeout stays at its default. The spawns that expect a broken fixture
 * keep calling maelys_mcp_provider_spawn: a deadline cannot turn their
 * failure into a pass, and they keep the default-path wrapper covered.
 */
static maelys_mcp_result_t spawn_fixture(const char *path, size_t limit,
    maelys_mcp_provider_t **out_provider, char **out_error) {
    maelys_mcp_provider_process_options_t options = {
        .executable_path = path,
        .max_message_bytes = limit,
        .describe_timeout_ms = 30000u
    };
    return maelys_mcp_provider_spawn_with_options(&options, out_provider,
        out_error);
}

int main(int argc, char **argv) {
    ASSERT_TRUE(argc == 10);
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn("/usr/bin/false", 65536u, &provider, &error) != MAELYS_MCP_OK);
    ASSERT_TRUE(provider == NULL);
    free(error);
    error = NULL;
    for (int index = 2; index <= 5; ++index) {
        size_t limit = index == 5 ? 256u : 65536u;
        ASSERT_TRUE(maelys_mcp_provider_spawn(argv[index], limit, &provider, &error) != MAELYS_MCP_OK);
        ASSERT_TRUE(provider == NULL);
        ASSERT_TRUE(error != NULL);
        free(error);
        error = NULL;
    }
    ASSERT_TRUE(spawn_fixture(argv[6], 65536u, &provider, &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(provider != NULL);
    free(error);
    error = NULL;
    maelys_mcp_provider_destroy(provider);
    provider = NULL;
    ASSERT_TRUE(spawn_fixture(argv[1], 65536u, &provider, &error) == MAELYS_MCP_OK);
    maelys_mcp_provider_t *fd_check = NULL;
    ASSERT_TRUE(spawn_fixture(argv[8], 65536u, &fd_check, &error) == MAELYS_MCP_OK);
    maelys_mcp_provider_destroy(fd_check);
    maelys_mcp_provider_destroy(provider);
    provider = NULL;
    maelys_mcp_provider_process_options_t short_timeout = {
        .executable_path = argv[7], .max_message_bytes = 65536u,
        .describe_timeout_ms = 50u, .call_timeout_ms = 50u,
        .shutdown_timeout_ms = 50u
    };
    long long started = milliseconds();
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_options(
        &short_timeout, &provider, &error) != MAELYS_MCP_OK);
    ASSERT_TRUE(provider == NULL && milliseconds() - started < 1000LL);
    free(error);
    error = NULL;
    short_timeout.executable_path = argv[9];
    /* This case tests the 50ms shutdown deadline through the destroy timing
     * below; its describe deadline is only a liveness bound, so it gets the
     * same loose one spawn_fixture uses. */
    short_timeout.describe_timeout_ms = 30000u;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_options(
        &short_timeout, &provider, &error) == MAELYS_MCP_OK);
    started = milliseconds();
    maelys_mcp_provider_destroy(provider);
    ASSERT_TRUE(milliseconds() - started < 1000LL);
    provider = NULL;
    ASSERT_TRUE(spawn_fixture(argv[1], 65536u, &provider, &error) == MAELYS_MCP_OK);
    free(error);
    maelys_mcp_runtime_config_t config = {
        .server_name = "process-test",
        .server_version = "1.0"
    };
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(maelys_mcp_runtime_create(&config, &runtime) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_TOOLS) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_MRTR) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_RESOURCES) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_SUBSCRIPTIONS) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, provider, NULL) == MAELYS_MCP_OK);
    maelys_mcp_channel_config_t channel_config = {
        .max_messages = 16u, .max_bytes = 65536u, .response_burst = 2u,
        .admission_timeout_ms = 2000u, .close_timeout_ms = 2000u
    };
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, &channel_config, &channel) ==
        MAELYS_MCP_OK);

    json_t *subscription_params = json_pack(
        "{s:{s:b,s:b,s:[s]},s:{s:s,s:{s:s,s:s},s:{}}}",
        "notifications",
            "toolsListChanged", 1,
            "resourcesListChanged", 1,
            "resourceSubscriptions", "example://about",
        "_meta",
            "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
            "io.modelcontextprotocol/clientInfo", "name", "test", "version", "1",
            "io.modelcontextprotocol/clientCapabilities");
    json_t *subscription_request = json_pack("{s:s,s:i,s:s,s:o}",
        "jsonrpc", "2.0", "id", 77, "method", "subscriptions/listen",
        "params", subscription_params);
    ASSERT_TRUE(maelys_mcp_channel_handle(channel, subscription_request) == MAELYS_MCP_OK);
    json_decref(subscription_request);
    json_t *acknowledged = next_message(channel);
    ASSERT_TRUE(acknowledged != NULL);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(acknowledged, "method")),
        "notifications/subscriptions/acknowledged") == 0);
    json_decref(acknowledged);

    json_t *params = json_pack("{s:s,s:{s:s},s:{s:s,s:{s:s,s:s},s:{}}}",
        "name", "example.echo",
        "arguments", "message", "external",
        "_meta",
            "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
            "io.modelcontextprotocol/clientInfo", "name", "test", "version", "1",
            "io.modelcontextprotocol/clientCapabilities");
    json_t *request = json_pack("{s:s,s:i,s:s,s:o}",
        "jsonrpc", "2.0", "id", 1, "method", "tools/call", "params", params);
    json_t *response = dispatch(channel, request);
    json_t *result = json_object_get(response, "result");
    json_t *structured = json_object_get(result, "structuredContent");
    ASSERT_TRUE(json_is_object(structured));
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(structured, "message")), "external") == 0);
    json_decref(response);

    params = json_pack("{s:s,s:{s:s,s:{s:s,s:s},s:{}}}",
        "uri", "example://missing",
        "_meta",
            "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
            "io.modelcontextprotocol/clientInfo", "name", "test", "version", "1",
            "io.modelcontextprotocol/clientCapabilities");
    request = json_pack("{s:s,s:i,s:s,s:o}",
        "jsonrpc", "2.0", "id", 3, "method", "resources/read", "params", params);
    response = dispatch(channel, request);
    json_t *response_error = json_object_get(response, "error");
    ASSERT_TRUE(json_integer_value(json_object_get(response_error, "code")) == -32602);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(
        json_object_get(response_error, "data"), "uri")), "example://missing") == 0);
    json_decref(response);

    params = json_pack("{s:s,s:{s:s,s:{s:s,s:s},s:{}}}",
        "uri", "example://echo/external-resource",
        "_meta",
            "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
            "io.modelcontextprotocol/clientInfo", "name", "test", "version", "1",
            "io.modelcontextprotocol/clientCapabilities");
    request = json_pack("{s:s,s:i,s:s,s:o}",
        "jsonrpc", "2.0", "id", 2, "method", "resources/read", "params", params);
    response = dispatch(channel, request);
    result = json_object_get(response, "result");
    json_t *contents = json_object_get(result, "contents");
    ASSERT_TRUE(json_array_size(contents) == 1u);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(json_array_get(contents, 0), "text")),
        "external-resource") == 0);
    json_decref(response);

    /*
     * Progress across the process boundary. The strict part is the ordering:
     * the reader queues frames, but the thread inside the call drains and
     * emits them, so they land in the ordered lane ahead of the response
     * rather than racing it. A provider never names a token - the host holds
     * it - so this also confirms the host attaches the right one.
     */
    params = json_pack("{s:s,s:{},s:{s:s,s:{s:s,s:s},s:{},s:s}}",
        "name", "example.progress",
        "arguments",
        "_meta",
            "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
            "io.modelcontextprotocol/clientInfo", "name", "test", "version", "1",
            "io.modelcontextprotocol/clientCapabilities",
            "progressToken", "wire-token");
    request = json_pack("{s:s,s:i,s:s,s:o}",
        "jsonrpc", "2.0", "id", 7, "method", "tools/call", "params", params);
    ASSERT_TRUE(maelys_mcp_channel_handle(channel, request) == MAELYS_MCP_OK);
    json_decref(request);
    for (int step = 0; step <= 2; ++step) {
        json_t *frame = next_message(channel);
        ASSERT_TRUE(frame != NULL);
        json_t *frame_method = json_object_get(frame, "method");
        ASSERT_TRUE(json_is_string(frame_method));
        ASSERT_TRUE(strcmp(json_string_value(frame_method), "notifications/progress") == 0);
        json_t *frame_params = json_object_get(frame, "params");
        ASSERT_TRUE(strcmp(json_string_value(
            json_object_get(frame_params, "progressToken")), "wire-token") == 0);
        ASSERT_TRUE(json_number_value(json_object_get(frame_params, "progress")) == step * 50.0);
        ASSERT_TRUE(json_number_value(json_object_get(frame_params, "total")) == 100.0);
        json_decref(frame);
    }
    response = next_message(channel);
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(json_integer_value(json_object_get(response, "id")) == 7);
    json_decref(response);

    /* No token, no frames: the very next message is the response itself. */
    params = json_pack("{s:s,s:{},s:{s:s,s:{s:s,s:s},s:{}}}",
        "name", "example.progress",
        "arguments",
        "_meta",
            "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
            "io.modelcontextprotocol/clientInfo", "name", "test", "version", "1",
            "io.modelcontextprotocol/clientCapabilities");
    request = json_pack("{s:s,s:i,s:s,s:o}",
        "jsonrpc", "2.0", "id", 8, "method", "tools/call", "params", params);
    ASSERT_TRUE(maelys_mcp_channel_handle(channel, request) == MAELYS_MCP_OK);
    json_decref(request);
    response = next_message(channel);
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(json_integer_value(json_object_get(response, "id")) == 8);
    json_decref(response);

    params = json_pack("{s:s,s:{},s:{s:s,s:{s:s,s:s},s:{}}}",
        "name", "example.events",
        "arguments",
        "_meta",
            "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
            "io.modelcontextprotocol/clientInfo", "name", "test", "version", "1",
            "io.modelcontextprotocol/clientCapabilities");
    request = json_pack("{s:s,s:i,s:s,s:o}",
        "jsonrpc", "2.0", "id", 4, "method", "tools/call", "params", params);
    ASSERT_TRUE(maelys_mcp_channel_handle(channel, request) == MAELYS_MCP_OK);
    json_decref(request);
    response = NULL;
    json_t *events[3] = {0};
    size_t event_count = 0u;
    for (size_t index = 0; index < 4u; ++index) {
        json_t *message = next_message(channel);
        ASSERT_TRUE(message != NULL);
        if (json_integer_value(json_object_get(message, "id")) == 4) {
            response = message;
        } else {
            ASSERT_TRUE(event_count < 3u);
            events[event_count++] = message;
        }
    }
    ASSERT_TRUE(response != NULL && event_count == 3u);
    result = json_object_get(response, "result");
    structured = json_object_get(result, "structuredContent");
    ASSERT_TRUE(json_integer_value(json_object_get(structured, "emitted")) == 3);
    json_decref(response);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(events[0], "method")),
        "notifications/resources/updated") == 0);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(events[1], "method")),
        "notifications/resources/list_changed") == 0);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(events[2], "method")),
        "notifications/tools/list_changed") == 0);

    for (size_t index = 0; index < 3u; ++index) json_decref(events[index]);
    ASSERT_TRUE(maelys_mcp_channel_complete_subscriptions(channel) == MAELYS_MCP_OK);
    json_t *complete = next_message(channel);
    ASSERT_TRUE(complete != NULL);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(
        json_object_get(complete, "result"), "resultType")), "complete") == 0);
    json_decref(complete);
    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    puts("test_process_provider: OK");
    return 0;
}
