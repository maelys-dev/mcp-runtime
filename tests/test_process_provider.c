#include "maelys/mcp.h"

#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ASSERT_TRUE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Assertion failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct event_capture {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    json_t *messages[8];
    size_t count;
} event_capture_t;

static maelys_mcp_result_t capture_event(void *context, json_t *message) {
    event_capture_t *capture = context;
    json_t *copy = json_deep_copy(message);
    if (!copy) return MAELYS_MCP_ERR_MEMORY;
    pthread_mutex_lock(&capture->mutex);
    if (capture->count == sizeof(capture->messages) / sizeof(capture->messages[0])) {
        pthread_mutex_unlock(&capture->mutex);
        json_decref(copy);
        return MAELYS_MCP_ERR_IO;
    }
    capture->messages[capture->count++] = copy;
    pthread_cond_broadcast(&capture->ready);
    pthread_mutex_unlock(&capture->mutex);
    return MAELYS_MCP_OK;
}

static int wait_for_messages(event_capture_t *capture, size_t expected) {
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return 0;
    deadline.tv_sec += 2;
    pthread_mutex_lock(&capture->mutex);
    while (capture->count < expected) {
        if (pthread_cond_timedwait(&capture->ready,
            &capture->mutex, &deadline) != 0) break;
    }
    int ready = capture->count >= expected;
    pthread_mutex_unlock(&capture->mutex);
    return ready;
}

static long long milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (long long)now.tv_sec * 1000LL + (long long)now.tv_nsec / 1000000LL;
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
    ASSERT_TRUE(maelys_mcp_provider_spawn(argv[6], 65536u, &provider, &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(provider != NULL);
    free(error);
    error = NULL;
    maelys_mcp_provider_destroy(provider);
    provider = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn(argv[1], 65536u, &provider, &error) == MAELYS_MCP_OK);
    maelys_mcp_provider_t *fd_check = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn(argv[8], 65536u, &fd_check, &error) == MAELYS_MCP_OK);
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
    short_timeout.describe_timeout_ms = 500u;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_options(
        &short_timeout, &provider, &error) == MAELYS_MCP_OK);
    started = milliseconds();
    maelys_mcp_provider_destroy(provider);
    ASSERT_TRUE(milliseconds() - started < 1000LL);
    provider = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn(argv[1], 65536u, &provider, &error) == MAELYS_MCP_OK);
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
    event_capture_t capture = {0};
    ASSERT_TRUE(pthread_mutex_init(&capture.mutex, NULL) == 0);
    ASSERT_TRUE(pthread_cond_init(&capture.ready, NULL) == 0);
    maelys_mcp_outbox_config_t outbox_config = {
        .max_messages = 16u, .max_bytes = 65536u, .batch_size = 4u,
        .response_burst = 2u, .write = capture_event, .write_context = &capture
    };
    maelys_mcp_outbox_t *outbox = NULL;
    ASSERT_TRUE(maelys_mcp_outbox_create(&outbox_config, &outbox) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_attach_outbox(runtime, outbox) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, provider, NULL) == MAELYS_MCP_OK);

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
    ASSERT_TRUE(maelys_mcp_runtime_handle(runtime, subscription_request) == NULL);
    json_decref(subscription_request);
    ASSERT_TRUE(wait_for_messages(&capture, 1u));

    json_t *params = json_pack("{s:s,s:{s:s},s:{s:s,s:{s:s,s:s},s:{}}}",
        "name", "example.echo",
        "arguments", "message", "external",
        "_meta",
            "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
            "io.modelcontextprotocol/clientInfo", "name", "test", "version", "1",
            "io.modelcontextprotocol/clientCapabilities");
    json_t *request = json_pack("{s:s,s:i,s:s,s:o}",
        "jsonrpc", "2.0", "id", 1, "method", "tools/call", "params", params);
    json_t *response = maelys_mcp_runtime_handle(runtime, request);
    json_t *result = json_object_get(response, "result");
    json_t *structured = json_object_get(result, "structuredContent");
    ASSERT_TRUE(json_is_object(structured));
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(structured, "message")), "external") == 0);
    json_decref(response);
    json_decref(request);

    params = json_pack("{s:s,s:{s:s,s:{s:s,s:s},s:{}}}",
        "uri", "example://missing",
        "_meta",
            "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
            "io.modelcontextprotocol/clientInfo", "name", "test", "version", "1",
            "io.modelcontextprotocol/clientCapabilities");
    request = json_pack("{s:s,s:i,s:s,s:o}",
        "jsonrpc", "2.0", "id", 3, "method", "resources/read", "params", params);
    response = maelys_mcp_runtime_handle(runtime, request);
    json_t *response_error = json_object_get(response, "error");
    ASSERT_TRUE(json_integer_value(json_object_get(response_error, "code")) == -32602);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(
        json_object_get(response_error, "data"), "uri")), "example://missing") == 0);
    json_decref(response);
    json_decref(request);

    params = json_pack("{s:s,s:{s:s,s:{s:s,s:s},s:{}}}",
        "uri", "example://echo/external-resource",
        "_meta",
            "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
            "io.modelcontextprotocol/clientInfo", "name", "test", "version", "1",
            "io.modelcontextprotocol/clientCapabilities");
    request = json_pack("{s:s,s:i,s:s,s:o}",
        "jsonrpc", "2.0", "id", 2, "method", "resources/read", "params", params);
    response = maelys_mcp_runtime_handle(runtime, request);
    result = json_object_get(response, "result");
    json_t *contents = json_object_get(result, "contents");
    ASSERT_TRUE(json_array_size(contents) == 1u);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(json_array_get(contents, 0), "text")),
        "external-resource") == 0);
    json_decref(response);
    json_decref(request);

    params = json_pack("{s:s,s:{},s:{s:s,s:{s:s,s:s},s:{}}}",
        "name", "example.events",
        "arguments",
        "_meta",
            "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
            "io.modelcontextprotocol/clientInfo", "name", "test", "version", "1",
            "io.modelcontextprotocol/clientCapabilities");
    request = json_pack("{s:s,s:i,s:s,s:o}",
        "jsonrpc", "2.0", "id", 4, "method", "tools/call", "params", params);
    response = maelys_mcp_runtime_handle(runtime, request);
    result = json_object_get(response, "result");
    structured = json_object_get(result, "structuredContent");
    ASSERT_TRUE(json_integer_value(json_object_get(structured, "emitted")) == 3);
    json_decref(response);
    json_decref(request);
    ASSERT_TRUE(wait_for_messages(&capture, 4u));
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(capture.messages[1], "method")),
        "notifications/resources/updated") == 0);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(capture.messages[2], "method")),
        "notifications/resources/list_changed") == 0);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(capture.messages[3], "method")),
        "notifications/tools/list_changed") == 0);

    maelys_mcp_runtime_detach_outbox(runtime);
    maelys_mcp_runtime_destroy(runtime);
    ASSERT_TRUE(maelys_mcp_outbox_destroy(outbox, 1) == MAELYS_MCP_OK);
    for (size_t index = 0; index < capture.count; ++index) {
        json_decref(capture.messages[index]);
    }
    pthread_cond_destroy(&capture.ready);
    pthread_mutex_destroy(&capture.mutex);
    puts("test_process_provider: OK");
    return 0;
}
