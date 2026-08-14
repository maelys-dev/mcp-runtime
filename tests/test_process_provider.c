#include "maelys/mcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_TRUE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Assertion failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(int argc, char **argv) {
    ASSERT_TRUE(argc == 6);
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn("/usr/bin/false", 65536u, &provider, &error) != MAELYS_MCP_OK);
    ASSERT_TRUE(provider == NULL);
    free(error);
    error = NULL;
    for (int index = 2; index < argc; ++index) {
        size_t limit = index == 5 ? 256u : 65536u;
        ASSERT_TRUE(maelys_mcp_provider_spawn(argv[index], limit, &provider, &error) != MAELYS_MCP_OK);
        ASSERT_TRUE(provider == NULL);
        ASSERT_TRUE(error != NULL);
        free(error);
        error = NULL;
    }
    ASSERT_TRUE(maelys_mcp_provider_spawn(argv[1], 65536u, &provider, &error) == MAELYS_MCP_OK);
    free(error);
    maelys_mcp_runtime_config_t config = {
        .server_name = "process-test",
        .server_version = "1.0"
    };
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(maelys_mcp_runtime_create(&config, &runtime) == MAELYS_MCP_OK);
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
