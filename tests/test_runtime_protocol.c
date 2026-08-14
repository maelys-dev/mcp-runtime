#include "maelys/mcp.h"
#include "tests/test_support.h"

#include <stdlib.h>
#include <string.h>

static maelys_mcp_runtime_t *new_runtime(void) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "protocol-test",
        .server_version = "1.0",
        .max_providers = 2
    };
    maelys_mcp_runtime_t *runtime = NULL;
    return maelys_mcp_runtime_create(&config, &runtime) == MAELYS_MCP_OK ? runtime : NULL;
}

static json_t *modern_params(void) {
    return json_pack("{s:{s:s,s:{s:s,s:s},s:{}}}",
        "_meta",
        "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
        "io.modelcontextprotocol/clientInfo", "name", "test", "version", "1",
        "io.modelcontextprotocol/clientCapabilities");
}

static json_t *request(json_t *id, const char *method, json_t *params) {
    json_t *value = json_object();
    if (!value || json_object_set_new(value, "jsonrpc", json_string("2.0")) != 0 ||
        (id && json_object_set_new(value, "id", id) != 0) ||
        json_object_set_new(value, "method", json_string(method)) != 0 ||
        json_object_set_new(value, "params", params ? params : json_object()) != 0) {
        if (value) json_decref(value);
        return NULL;
    }
    return value;
}

static int error_code(json_t *response) {
    return (int)json_integer_value(json_object_get(json_object_get(response, "error"), "code"));
}

static json_t *dispatch(maelys_mcp_runtime_t *runtime, json_t *owned_request) {
    json_t *response = maelys_mcp_runtime_handle(runtime, owned_request);
    json_decref(owned_request);
    return response;
}

static int test_invalid_envelopes_and_ids(void) {
    maelys_mcp_runtime_t *runtime = new_runtime();
    ASSERT_TRUE(runtime != NULL);
    json_t *array_request = json_array();
    json_t *response = maelys_mcp_runtime_handle(runtime, array_request);
    ASSERT_TRUE(error_code(response) == -32600);
    json_decref(response);
    json_decref(array_request);
    json_t *invalid[] = {
        json_pack("{s:s,s:i,s:s}", "jsonrpc", "1.0", "id", 1, "method", "x"),
        json_pack("{s:s,s:i,s:i}", "jsonrpc", "2.0", "id", 1, "method", 2),
        json_pack("{s:s,s:b,s:s}", "jsonrpc", "2.0", "id", 1, "method", "x"),
        json_pack("{s:s,s:n,s:s}", "jsonrpc", "2.0", "id", "method", "x")
    };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        ASSERT_TRUE(invalid[index] != NULL);
        response = maelys_mcp_runtime_handle(runtime, invalid[index]);
        ASSERT_TRUE(error_code(response) == -32600);
        if (index >= 2) ASSERT_TRUE(json_is_null(json_object_get(response, "id")));
        json_decref(response);
        json_decref(invalid[index]);
    }
    maelys_mcp_runtime_destroy(runtime);
    return 0;
}

static int test_notifications_have_no_response_or_side_effect(void) {
    maelys_mcp_runtime_t *runtime = new_runtime();
    ASSERT_TRUE(runtime != NULL);
    json_t *notification = request(NULL, "tools/call", modern_params());
    ASSERT_TRUE(notification != NULL);
    ASSERT_TRUE(maelys_mcp_runtime_handle(runtime, notification) == NULL);
    json_decref(notification);
    maelys_mcp_runtime_destroy(runtime);
    return 0;
}

static int test_modern_metadata_contract(void) {
    maelys_mcp_runtime_t *runtime = new_runtime();
    ASSERT_TRUE(runtime != NULL);
    json_t *response = dispatch(runtime,
        request(json_integer(1), "server/discover", json_object()));
    ASSERT_TRUE(error_code(response) == -32602);
    json_decref(response);

    json_t *params = modern_params();
    json_object_set_new(json_object_get(params, "_meta"),
        "io.modelcontextprotocol/clientInfo", json_pack("{s:s}", "name", "missing-version"));
    response = dispatch(runtime,
        request(json_string("abc"), "tools/list", params));
    ASSERT_TRUE(error_code(response) == -32602);
    ASSERT_TRUE(json_is_string(json_object_get(response, "id")));
    json_decref(response);

    params = modern_params();
    json_object_set_new(json_object_get(params, "_meta"),
        "io.modelcontextprotocol/protocolVersion", json_string("2099-01-01"));
    response = dispatch(runtime,
        request(json_integer(3), "tools/list", params));
    ASSERT_TRUE(error_code(response) == -32022);
    json_decref(response);
    maelys_mcp_runtime_destroy(runtime);
    return 0;
}

static int test_legacy_lifecycle_and_unknown_method(void) {
    maelys_mcp_runtime_t *runtime = new_runtime();
    ASSERT_TRUE(runtime != NULL);
    json_t *response = dispatch(runtime,
        request(json_integer(1), "tools/list", json_object()));
    ASSERT_TRUE(error_code(response) == -32002);
    json_decref(response);

    json_t *bad_init = json_pack("{s:s}", "protocolVersion", "old");
    response = dispatch(runtime,
        request(json_integer(2), "initialize", bad_init));
    ASSERT_TRUE(error_code(response) == -32602);
    json_decref(response);

    json_t *init = json_pack("{s:s,s:{},s:{s:s,s:s}}",
        "protocolVersion", MAELYS_MCP_PROTOCOL_LEGACY, "capabilities",
        "clientInfo", "name", "client", "version", "1");
    response = dispatch(runtime,
        request(json_integer(3), "initialize", init));
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);
    response = dispatch(runtime,
        request(json_integer(4), "initialize", json_object()));
    ASSERT_TRUE(error_code(response) == -32600);
    json_decref(response);
    json_t *initialized = request(NULL, "notifications/initialized", json_object());
    ASSERT_TRUE(maelys_mcp_runtime_handle(runtime, initialized) == NULL);
    json_decref(initialized);
    response = dispatch(runtime,
        request(json_integer(5), "unknown/method", json_object()));
    ASSERT_TRUE(error_code(response) == -32601);
    json_decref(response);
    maelys_mcp_runtime_destroy(runtime);
    return 0;
}

static maelys_mcp_result_t invalid_output_call(
    void *context,
    const char *tool_name,
    json_t *arguments,
    json_t **out_result,
    char **out_error) {
    (void)context; (void)tool_name; (void)arguments; (void)out_error;
    *out_result = json_pack("{s:s}", "ok", "not-a-boolean");
    return *out_result ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static int test_output_schema_failure_is_fail_closed(void) {
    maelys_mcp_runtime_t *runtime = new_runtime();
    ASSERT_TRUE(runtime != NULL);
    json_t *input = json_pack("{s:s,s:b}", "type", "object", "additionalProperties", 0);
    json_t *output = json_pack("{s:s,s:{s:{s:s}},s:[s],s:b}",
        "type", "object", "properties", "ok", "type", "boolean",
        "required", "ok", "additionalProperties", 0);
    maelys_mcp_tool_t tool = {
        .name = "test.invalid-output", .description = "invalid output",
        .input_schema = input, .output_schema = output, .effect = MAELYS_MCP_EFFECT_READ
    };
    maelys_mcp_provider_config_t config = {
        .name = "test", .version = "1", .tools = &tool, .tool_count = 1,
        .call = invalid_output_call
    };
    maelys_mcp_provider_t *provider = NULL;
    ASSERT_TRUE(maelys_mcp_provider_create(&config, &provider) == MAELYS_MCP_OK);
    json_decref(input);
    json_decref(output);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, provider, NULL) == MAELYS_MCP_OK);
    json_t *params = modern_params();
    json_object_set_new(params, "name", json_string("test.invalid-output"));
    json_object_set_new(params, "arguments", json_object());
    json_t *response = dispatch(runtime,
        request(json_integer(1), "tools/call", params));
    json_t *result = json_object_get(response, "result");
    ASSERT_TRUE(json_is_true(json_object_get(result, "isError")));
    ASSERT_TRUE(json_object_get(result, "structuredContent") == NULL);
    json_decref(response);
    maelys_mcp_runtime_destroy(runtime);
    return 0;
}

int main(void) {
    static const maelys_test_case_t tests[] = {
        {"invalid JSON-RPC envelopes and id types", test_invalid_envelopes_and_ids},
        {"notifications do not produce responses", test_notifications_have_no_response_or_side_effect},
        {"modern metadata and version contract", test_modern_metadata_contract},
        {"legacy initialization lifecycle and unknown method", test_legacy_lifecycle_and_unknown_method},
        {"invalid provider output is fail-closed", test_output_schema_failure_is_fail_closed}
    };
    return maelys_run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
