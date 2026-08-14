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

typedef struct test_context {
    int calls;
    int audits;
} test_context_t;

static maelys_mcp_result_t call_tool(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    (void)out_error;
    test_context_t *state = context;
    state->calls++;
    if (strcmp(request->tool_name, "test.null") == 0) {
        return MAELYS_MCP_OK;
    }
    json_t *message = json_object_get(request->arguments, "message");
    out_result->structured_content = json_pack("{s:s,s:s}", "tool", request->tool_name,
        "message", json_is_string(message) ? json_string_value(message) : "mutated");
    return out_result->structured_content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static int authorize(void *context, const maelys_mcp_request_context_t *request) {
    (void)context;
    return request->effect == MAELYS_MCP_EFFECT_READ ||
        request->effect == MAELYS_MCP_EFFECT_PREVIEW;
}

static void audit_call(
    void *context,
    const maelys_mcp_request_context_t *request,
    maelys_mcp_result_t outcome) {
    (void)request;
    (void)outcome;
    test_context_t *state = context;
    state->audits++;
}

static json_t *modern_params(void) {
    return json_pack("{s:{s:s,s:{s:s,s:s},s:{}}}",
        "_meta",
        "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
        "io.modelcontextprotocol/clientInfo", "name", "test", "version", "1.0",
        "io.modelcontextprotocol/clientCapabilities");
}

static json_t *request(json_int_t id, const char *method, json_t *params) {
    return json_pack("{s:s,s:I,s:s,s:o}",
        "jsonrpc", "2.0", "id", id, "method", method,
        "params", params ? params : json_object());
}

static json_t *dispatch(maelys_mcp_runtime_t *runtime, json_t *value) {
    json_t *response = maelys_mcp_runtime_handle(runtime, value);
    json_decref(value);
    return response;
}

static json_t *response_result(json_t *response) {
    if (!json_is_object(response) || json_object_get(response, "error")) {
        char *encoded = response ? json_dumps(response, JSON_COMPACT) : NULL;
        fprintf(stderr, "Expected success response, got: %s\n", encoded ? encoded : "null");
        free(encoded);
        return NULL;
    }
    return json_object_get(response, "result");
}

int main(void) {
    test_context_t state = {0};
    ASSERT_TRUE(strcmp(maelys_mcp_version_string(), "0.6.1") == 0);
    ASSERT_TRUE(maelys_mcp_abi_version() == MAELYS_MCP_ABI_VERSION);
    maelys_mcp_tool_effect_t parsed_effect = MAELYS_MCP_EFFECT_UNSPECIFIED;
    ASSERT_TRUE(maelys_mcp_tool_effect_parse("commit", &parsed_effect) == MAELYS_MCP_OK);
    ASSERT_TRUE(parsed_effect == MAELYS_MCP_EFFECT_COMMIT);
    ASSERT_TRUE(strcmp(maelys_mcp_tool_effect_string(parsed_effect), "commit") == 0);
    ASSERT_TRUE(maelys_mcp_tool_effect_parse("mutation", &parsed_effect) == MAELYS_MCP_ERR_ARGUMENT);
    json_t *read_schema = json_pack("{s:s,s:{s:{s:s,s:i}},s:[s],s:b}",
        "type", "object", "properties", "message", "type", "string", "minLength", 1,
        "required", "message", "additionalProperties", 0);
    json_t *empty_schema = json_pack("{s:s,s:b}", "type", "object", "additionalProperties", 0);
    ASSERT_TRUE(read_schema && empty_schema);
    maelys_mcp_tool_t invalid_tool = {
        .name = "test.unspecified",
        .description = "Missing a mandatory effect.",
        .input_schema = empty_schema
    };
    maelys_mcp_provider_config_t invalid_config = {
        .name = "invalid",
        .version = "1.0",
        .tools = &invalid_tool,
        .tool_count = 1,
        .call = call_tool,
        .context = &state
    };
    maelys_mcp_provider_t *invalid_provider = NULL;
    ASSERT_TRUE(maelys_mcp_provider_create(&invalid_config, &invalid_provider) ==
        MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(!invalid_provider);
    maelys_mcp_tool_t tools[] = {
        {
            .name = "test.echo",
            .title = "Echo",
            .description = "Echoes a message.",
            .input_schema = read_schema,
            .effect = MAELYS_MCP_EFFECT_READ
        },
        {
            .name = "test.mutate",
            .title = "Mutate",
            .description = "Mutation used to verify policy denial.",
            .input_schema = empty_schema,
            .effect = MAELYS_MCP_EFFECT_APPLY
        },
        {
            .name = "test.null",
            .title = "Invalid provider result",
            .description = "Returns no result to verify fail-closed handling.",
            .input_schema = empty_schema,
            .effect = MAELYS_MCP_EFFECT_READ
        }
    };
    maelys_mcp_provider_config_t provider_config = {
        .name = "test",
        .version = "1.0",
        .tools = tools,
        .tool_count = 3,
        .call = call_tool,
        .context = &state
    };
    maelys_mcp_provider_t *provider = NULL;
    ASSERT_TRUE(maelys_mcp_provider_create(&provider_config, &provider) == MAELYS_MCP_OK);
    json_decref(read_schema);
    json_decref(empty_schema);

    maelys_mcp_runtime_config_t runtime_config = {
        .server_name = "test-runtime",
        .server_version = "1.0",
        .instructions = "test",
        .authorize = authorize,
        .audit = audit_call,
        .policy_context = &state
    };
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(maelys_mcp_runtime_create(&runtime_config, &runtime) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_TOOLS) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_MRTR) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, provider, NULL) == MAELYS_MCP_OK);

    json_t *notification_params = modern_params();
    json_object_set_new(notification_params, "name", json_string("test.echo"));
    json_object_set_new(notification_params, "arguments", json_pack("{s:s}", "message", "ignored"));
    json_t *notification = json_pack("{s:s,s:s,s:o}",
        "jsonrpc", "2.0", "method", "tools/call", "params", notification_params);
    ASSERT_TRUE(dispatch(runtime, notification) == NULL);
    ASSERT_TRUE(state.calls == 0);

    json_t *response = dispatch(runtime,
        request(1, "server/discover", modern_params()));
    json_t *result = response_result(response);
    ASSERT_TRUE(result);
    ASSERT_TRUE(json_array_size(json_object_get(result, "supportedVersions")) == 2);
    ASSERT_TRUE(json_object_get(result, "_meta"));
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(result, "resultType")), "complete") == 0);
    json_decref(response);

    response = dispatch(runtime, request(2, "tools/list", modern_params()));
    result = response_result(response);
    ASSERT_TRUE(result);
    ASSERT_TRUE(json_array_size(json_object_get(result, "tools")) == 2);
    ASSERT_TRUE(json_integer_value(json_object_get(result, "ttlMs")) == 0);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(result, "resultType")), "complete") == 0);
    json_t *listed_tool = json_array_get(json_object_get(result, "tools"), 0);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(
        json_object_get(listed_tool, "_meta"), "org.maelys/toolEffect")), "read") == 0);
    json_decref(response);

    json_t *params = modern_params();
    json_object_set_new(params, "name", json_string("test.echo"));
    json_object_set_new(params, "arguments", json_pack("{s:s}", "message", "hello"));
    response = dispatch(runtime, request(3, "tools/call", params));
    result = response_result(response);
    ASSERT_TRUE(result);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(
        json_object_get(result, "structuredContent"), "message")), "hello") == 0);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(result, "resultType")), "complete") == 0);
    ASSERT_TRUE(state.calls == 1 && state.audits == 1);
    json_decref(response);

    params = modern_params();
    json_object_set_new(params, "name", json_string("test.echo"));
    json_object_set_new(params, "arguments", json_object());
    response = dispatch(runtime, request(4, "tools/call", params));
    ASSERT_TRUE(json_integer_value(json_object_get(json_object_get(response, "error"), "code")) == -32602);
    ASSERT_TRUE(state.calls == 1);
    json_decref(response);

    params = modern_params();
    json_object_set_new(params, "name", json_string("test.mutate"));
    json_object_set_new(params, "arguments", json_object());
    response = dispatch(runtime, request(5, "tools/call", params));
    ASSERT_TRUE(json_integer_value(json_object_get(json_object_get(response, "error"), "code")) == -32003);
    ASSERT_TRUE(state.calls == 1 && state.audits == 2);
    json_decref(response);

    params = modern_params();
    json_object_set_new(params, "name", json_string("test.null"));
    json_object_set_new(params, "arguments", json_object());
    response = dispatch(runtime, request(9, "tools/call", params));
    result = response_result(response);
    ASSERT_TRUE(result);
    ASSERT_TRUE(json_is_true(json_object_get(result, "isError")));
    ASSERT_TRUE(state.calls == 2 && state.audits == 3);
    json_decref(response);

    json_t *init = json_pack("{s:s,s:{},s:{s:s,s:s}}",
        "protocolVersion", MAELYS_MCP_PROTOCOL_LEGACY,
        "capabilities",
        "clientInfo", "name", "legacy-test", "version", "1");
    response = dispatch(runtime, request(6, "initialize", init));
    result = response_result(response);
    ASSERT_TRUE(result);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(result, "protocolVersion")),
        MAELYS_MCP_PROTOCOL_LEGACY) == 0);
    json_decref(response);
    ASSERT_TRUE(dispatch(runtime, request(0, "notifications/initialized", json_object())) == NULL);
    response = dispatch(runtime, request(7, "tools/list", json_object()));
    result = response_result(response);
    ASSERT_TRUE(result);
    ASSERT_TRUE(json_array_size(json_object_get(result, "tools")) == 2);
    json_decref(response);

    params = json_pack("{s:{s:s,s:{}}}", "_meta",
        "io.modelcontextprotocol/protocolVersion", "2099-01-01",
        "io.modelcontextprotocol/clientCapabilities");
    response = dispatch(runtime, request(8, "tools/list", params));
    ASSERT_TRUE(json_integer_value(json_object_get(json_object_get(response, "error"), "code")) == -32022);
    json_decref(response);

    maelys_mcp_runtime_destroy(runtime);
    puts("test_runtime: OK");
    return 0;
}
