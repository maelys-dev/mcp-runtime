#include "maelys/mcp.h"
#include "tests/test_support.h"

#include <stdlib.h>
#include <string.h>

typedef struct test_state {
    int saw_retry;
} test_state_t;

static maelys_mcp_result_t rich_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *result,
    char **out_error) {
    test_state_t *state = context;
    if (strcmp(request->tool_name, "test.rich") == 0) {
        result->content = json_pack("[{s:s,s:s},{s:s,s:s,s:s},{s:s,s:s,s:s},{s:s,s:{s:s,s:s}}]",
            "type", "text", "text", "hello",
            "type", "image", "data", "aGVsbG8=", "mimeType", "image/png",
            "type", "audio", "data", "aGVsbG8=", "mimeType", "audio/wav",
            "type", "resource", "resource", "uri", "test://document", "text", "body");
        result->structured_content = json_pack("{s:b}", "ok", 1);
        return result->content && result->structured_content ?
            MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
    }
    if (strcmp(request->tool_name, "test.invalid-content") == 0) {
        result->content = json_pack("[{s:s,s:s,s:s}]",
            "type", "image", "data", "not-base64", "mimeType", "image/png");
        return result->content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
    }
    if (strcmp(request->tool_name, "test.confirm") == 0) {
        if (!request->input_responses) {
            result->type = MAELYS_MCP_PROVIDER_RESULT_INPUT_REQUIRED;
            json_error_t parse_error;
            result->input_requests = json_loads(
                "{\"confirmation\":{\"method\":\"elicitation/create\",\"params\":{"
                "\"message\":\"Apply the change?\",\"requestedSchema\":{"
                "\"type\":\"object\",\"properties\":{\"accept\":{\"type\":\"boolean\"}},"
                "\"required\":[\"accept\"]}}}}", 0, &parse_error);
            result->request_state = json_string("state-1");
            return result->input_requests && result->request_state ?
                MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
        }
        if (!json_is_string(request->request_state) ||
            strcmp(json_string_value(request->request_state), "state-1") != 0) {
            if (out_error) *out_error = strdup("requestState validation failed");
            return MAELYS_MCP_ERR_DENIED;
        }
        state->saw_retry = 1;
        result->content = json_pack("[{s:s,s:s}]", "type", "text", "text", "confirmed");
        return result->content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
    }
    if (out_error) *out_error = strdup("unknown test tool");
    return MAELYS_MCP_ERR_NOT_FOUND;
}

static json_t *modern_params(void) {
    return json_pack("{s:{s:s,s:{s:s,s:s},s:{s:{}}}}",
        "_meta",
        "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
        "io.modelcontextprotocol/clientInfo", "name", "test", "version", "1",
        "io.modelcontextprotocol/clientCapabilities", "elicitation");
}

static json_t *request(const char *method, json_t *params) {
    return json_pack("{s:s,s:i,s:s,s:o}", "jsonrpc", "2.0", "id", 1,
        "method", method, "params", params);
}

static json_t *dispatch(maelys_mcp_runtime_t *runtime, const char *method, json_t *params) {
    json_t *owned_request = request(method, params);
    json_t *response = maelys_mcp_runtime_handle(runtime, owned_request);
    json_decref(owned_request);
    return response;
}

static int test_content_validation(void) {
    char *error = NULL;
    json_t *valid = json_pack("[{s:s,s:s},{s:s,s:s,s:s},{s:s,s:{s:s,s:s}}]",
        "type", "text", "text", "ok",
        "type", "audio", "data", "aGVsbG8=", "mimeType", "audio/wav",
        "type", "resource", "resource", "uri", "test://x", "text", "body");
    ASSERT_TRUE(maelys_mcp_validate_content(valid, 1024, &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(error == NULL);
    json_decref(valid);
    const char *invalid[] = {
        "[]",
        "[{\"type\":\"image\",\"data\":\"bad\",\"mimeType\":\"image/png\"}]",
        "[{\"type\":\"audio\",\"data\":\"aGVsbG8=\",\"mimeType\":\"image/png\"}]",
        "[{\"type\":\"resource\",\"resource\":{\"uri\":\"x\",\"text\":\"a\",\"blob\":\"Yg==\"}}]",
        "[{\"type\":\"unknown\"}]"
    };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        json_error_t parse_error;
        json_t *value = json_loads(invalid[index], 0, &parse_error);
        ASSERT_TRUE(value != NULL);
        ASSERT_TRUE(maelys_mcp_validate_content(value, 1024, &error) == MAELYS_MCP_ERR_PROTOCOL);
        ASSERT_TRUE(error != NULL);
        free(error);
        error = NULL;
        json_decref(value);
    }
    const char hostile_base64[] = {'a', 'G', 'V', 's', '\0', 'b', 'G', '8', '='};
    json_t *nul_data = json_stringn(hostile_base64, sizeof(hostile_base64));
    json_t *nul_block = json_pack("[{s:s,s:o,s:s}]",
        "type", "image", "data", nul_data, "mimeType", "image/png");
    ASSERT_TRUE(nul_block != NULL);
    ASSERT_TRUE(maelys_mcp_validate_content(nul_block, 1024, &error) == MAELYS_MCP_ERR_PROTOCOL);
    free(error);
    error = NULL;
    json_decref(nul_block);
    const char hostile_type[] = {'t', 'e', 'x', 't', '\0', 'x'};
    json_t *nul_type = json_pack("[{s:o,s:s}]", "type",
        json_stringn(hostile_type, sizeof(hostile_type)), "text", "unsafe");
    ASSERT_TRUE(nul_type != NULL);
    ASSERT_TRUE(maelys_mcp_validate_content(nul_type, 1024, &error) == MAELYS_MCP_ERR_PROTOCOL);
    free(error);
    json_decref(nul_type);
    return 0;
}

static int test_modules_rich_content_and_mrtr(void) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "module-test", .server_version = "0.3.0"
    };
    maelys_mcp_runtime_t *core = NULL;
    ASSERT_TRUE(maelys_mcp_runtime_create(&config, &core) == MAELYS_MCP_OK);
    json_t *response = dispatch(core, "server/discover", modern_params());
    json_t *capabilities = json_object_get(json_object_get(response, "result"), "capabilities");
    ASSERT_TRUE(json_object_size(capabilities) == 0u);
    json_decref(response);
    response = dispatch(core, "tools/list", modern_params());
    ASSERT_TRUE(json_integer_value(json_object_get(json_object_get(response, "error"), "code")) == -32601);
    json_decref(response);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(core, MAELYS_MCP_MODULE_TOOLS) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(core, MAELYS_MCP_MODULE_TOOLS) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_module_enabled(core, MAELYS_MCP_MODULE_TOOLS));
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(core, (maelys_mcp_module_kind_t)99) == MAELYS_MCP_ERR_ARGUMENT);

    test_state_t state = {0};
    json_t *schema = json_pack("{s:s,s:b}", "type", "object", "additionalProperties", 0);
    maelys_mcp_tool_t tools[] = {
        {.name = "test.rich", .description = "rich", .input_schema = schema,
            .effect = MAELYS_MCP_EFFECT_READ},
        {.name = "test.invalid-content", .description = "invalid", .input_schema = schema,
            .effect = MAELYS_MCP_EFFECT_READ},
        {.name = "test.confirm", .description = "MRTR", .input_schema = schema,
            .effect = MAELYS_MCP_EFFECT_APPLY}
    };
    maelys_mcp_provider_config_t provider_config = {
        .name = "test", .version = "1", .tools = tools, .tool_count = 3,
        .call = rich_call, .context = &state
    };
    maelys_mcp_provider_t *provider = NULL;
    ASSERT_TRUE(maelys_mcp_provider_create(&provider_config, &provider) == MAELYS_MCP_OK);
    json_decref(schema);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(core, provider, NULL) == MAELYS_MCP_OK);

    json_t *params = modern_params();
    json_object_set_new(params, "name", json_string("test.rich"));
    json_object_set_new(params, "arguments", json_object());
    response = dispatch(core, "tools/call", params);
    json_t *result = json_object_get(response, "result");
    ASSERT_TRUE(json_array_size(json_object_get(result, "content")) == 4u);
    ASSERT_TRUE(json_is_true(json_object_get(json_object_get(result, "structuredContent"), "ok")));
    json_decref(response);

    params = modern_params();
    json_object_set_new(params, "name", json_string("test.invalid-content"));
    json_object_set_new(params, "arguments", json_object());
    response = dispatch(core, "tools/call", params);
    ASSERT_TRUE(json_is_true(json_object_get(json_object_get(response, "result"), "isError")));
    json_decref(response);

    params = modern_params();
    json_object_set_new(params, "name", json_string("test.confirm"));
    json_object_set_new(params, "arguments", json_object());
    response = dispatch(core, "tools/call", params);
    ASSERT_TRUE(json_is_true(json_object_get(json_object_get(response, "result"), "isError")));
    json_decref(response);

    ASSERT_TRUE(maelys_mcp_runtime_enable_module(core, MAELYS_MCP_MODULE_MRTR) == MAELYS_MCP_OK);
    params = modern_params();
    json_object_del(json_object_get(json_object_get(params, "_meta"),
        "io.modelcontextprotocol/clientCapabilities"), "elicitation");
    json_object_set_new(params, "name", json_string("test.confirm"));
    json_object_set_new(params, "arguments", json_object());
    response = dispatch(core, "tools/call", params);
    ASSERT_TRUE(json_integer_value(json_object_get(json_object_get(response, "error"), "code")) == -32021);
    ASSERT_TRUE(json_is_object(json_object_get(json_object_get(json_object_get(response, "error"), "data"),
        "requiredCapabilities")));
    json_decref(response);

    params = modern_params();
    json_object_set_new(params, "name", json_string("test.confirm"));
    json_object_set_new(params, "arguments", json_object());
    response = dispatch(core, "tools/call", params);
    result = json_object_get(response, "result");
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(result, "resultType")), "input_required") == 0);
    ASSERT_TRUE(json_is_object(json_object_get(result, "inputRequests")));
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(result, "requestState")), "state-1") == 0);
    json_decref(response);

    params = modern_params();
    json_object_set_new(params, "name", json_string("test.confirm"));
    json_object_set_new(params, "arguments", json_object());
    json_object_set_new(params, "inputResponses", json_pack("{s:{s:s}}", "confirmation", "action", "accept"));
    json_object_set_new(params, "requestState", json_string("state-1"));
    response = dispatch(core, "tools/call", params);
    result = json_object_get(response, "result");
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(result, "resultType")), "complete") == 0);
    ASSERT_TRUE(state.saw_retry == 1);
    json_decref(response);
    maelys_mcp_runtime_destroy(core);
    return 0;
}

int main(void) {
    static const maelys_test_case_t tests[] = {
        {"content block validation", test_content_validation},
        {"module registry, rich content, and MRTR", test_modules_rich_content_and_mrtr}
    };
    return maelys_run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
