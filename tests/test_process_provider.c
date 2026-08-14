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
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, provider, NULL) == MAELYS_MCP_OK);

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
    maelys_mcp_runtime_destroy(runtime);
    puts("test_process_provider: OK");
    return 0;
}
