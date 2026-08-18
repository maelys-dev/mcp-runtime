#include "maelys/mcp.h"
#include "tests/test_support.h"

#include <stdlib.h>
#include <string.h>

typedef struct test_state {
    int saw_retry;
    int saw_client_capabilities;
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
        /*
         * Wire-level check, not just mcp-runtime's internal gate: a real
         * out-of-process provider only ever sees what travels over
         * maelys-provider/3, so the fix must put the legacy-declared
         * capability where the provider can actually read it.
         */
        if (json_is_object(request->client_capabilities) &&
            json_is_object(json_object_get(request->client_capabilities, "elicitation"))) {
            state->saw_client_capabilities = 1;
        }
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

static json_t *dispatch(maelys_mcp_channel_t *channel, const char *method, json_t *params) {
    json_t *owned_request = request(method, params);
    maelys_mcp_result_t status = maelys_mcp_channel_handle(channel, owned_request);
    json_decref(owned_request);
    if (status != MAELYS_MCP_OK) return NULL;
    json_t *response = NULL;
    if (maelys_mcp_channel_next(channel, 1000u, &response) != MAELYS_MCP_OK) {
        return NULL;
    }
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
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(core, NULL, &channel) == MAELYS_MCP_OK);
    json_t *response = dispatch(channel, "server/discover", modern_params());
    json_t *capabilities = json_object_get(json_object_get(response, "result"), "capabilities");
    ASSERT_TRUE(json_object_size(capabilities) == 0u);
    json_decref(response);
    response = dispatch(channel, "tools/list", modern_params());
    ASSERT_TRUE(json_integer_value(json_object_get(json_object_get(response, "error"), "code")) == -32601);
    json_decref(response);
    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(core) == MAELYS_MCP_OK);
    core = NULL;
    channel = NULL;
    ASSERT_TRUE(maelys_mcp_runtime_create(&config, &core) == MAELYS_MCP_OK);
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
    ASSERT_TRUE(maelys_mcp_channel_create(core, NULL, &channel) == MAELYS_MCP_OK);

    json_t *params = modern_params();
    json_object_set_new(params, "name", json_string("test.rich"));
    json_object_set_new(params, "arguments", json_object());
    response = dispatch(channel, "tools/call", params);
    json_t *result = json_object_get(response, "result");
    ASSERT_TRUE(json_array_size(json_object_get(result, "content")) == 4u);
    ASSERT_TRUE(json_is_true(json_object_get(json_object_get(result, "structuredContent"), "ok")));
    json_decref(response);

    params = modern_params();
    json_object_set_new(params, "name", json_string("test.invalid-content"));
    json_object_set_new(params, "arguments", json_object());
    response = dispatch(channel, "tools/call", params);
    ASSERT_TRUE(json_is_true(json_object_get(json_object_get(response, "result"), "isError")));
    json_decref(response);

    params = modern_params();
    json_object_set_new(params, "name", json_string("test.confirm"));
    json_object_set_new(params, "arguments", json_object());
    response = dispatch(channel, "tools/call", params);
    ASSERT_TRUE(json_is_true(json_object_get(json_object_get(response, "result"), "isError")));
    json_decref(response);

    ASSERT_TRUE(maelys_mcp_runtime_enable_module(core, MAELYS_MCP_MODULE_MRTR) == MAELYS_MCP_OK);
    params = modern_params();
    json_object_del(json_object_get(json_object_get(params, "_meta"),
        "io.modelcontextprotocol/clientCapabilities"), "elicitation");
    json_object_set_new(params, "name", json_string("test.confirm"));
    json_object_set_new(params, "arguments", json_object());
    response = dispatch(channel, "tools/call", params);
    ASSERT_TRUE(json_integer_value(json_object_get(json_object_get(response, "error"), "code")) == -32021);
    ASSERT_TRUE(json_is_object(json_object_get(json_object_get(json_object_get(response, "error"), "data"),
        "requiredCapabilities")));
    json_decref(response);

    params = modern_params();
    json_object_set_new(params, "name", json_string("test.confirm"));
    json_object_set_new(params, "arguments", json_object());
    response = dispatch(channel, "tools/call", params);
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
    response = dispatch(channel, "tools/call", params);
    result = json_object_get(response, "result");
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(result, "resultType")), "complete") == 0);
    ASSERT_TRUE(state.saw_retry == 1);
    json_decref(response);
    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(core) == MAELYS_MCP_OK);
    return 0;
}

/*
 * A legacy channel declares capabilities once, at initialize(), under the
 * same top-level keys (elicitation, sampling, roots) modern per-request _meta
 * carries. This must be enough to use input_required/MRTR under the client's
 * own negotiated legacy revision, per the spec's compatibility matrix — not
 * a blanket "legacy can never do MRTR" the way it used to be. A capability
 * the client genuinely never declared must still be refused explicitly.
 */
static int test_legacy_mrtr_capability_negotiation(void) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "legacy-mrtr-test", .server_version = "1.0"
    };
    maelys_mcp_runtime_t *core = NULL;
    ASSERT_TRUE(maelys_mcp_runtime_create(&config, &core) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(core, MAELYS_MCP_MODULE_TOOLS) == MAELYS_MCP_OK);

    test_state_t state = {0};
    json_t *schema = json_pack("{s:s,s:b}", "type", "object", "additionalProperties", 0);
    maelys_mcp_tool_t tool = {.name = "test.confirm", .description = "MRTR",
        .input_schema = schema, .effect = MAELYS_MCP_EFFECT_APPLY};
    maelys_mcp_provider_config_t provider_config = {
        .name = "test", .version = "1", .tools = &tool, .tool_count = 1,
        .call = rich_call, .context = &state
    };
    maelys_mcp_provider_t *provider = NULL;
    ASSERT_TRUE(maelys_mcp_provider_create(&provider_config, &provider) == MAELYS_MCP_OK);
    json_decref(schema);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(core, provider, NULL) == MAELYS_MCP_OK);

    /* Channel A: legacy, no elicitation declared. */
    maelys_mcp_channel_t *channel_a = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(core, NULL, &channel_a) == MAELYS_MCP_OK);
    json_t *init_a = json_pack("{s:s,s:{},s:{s:s,s:s}}",
        "protocolVersion", MAELYS_MCP_PROTOCOL_LEGACY, "capabilities",
        "clientInfo", "name", "legacy-no-caps", "version", "1");
    json_t *response = dispatch(channel_a, "initialize", init_a);
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);
    json_t *initialized = json_pack("{s:s,s:s}", "jsonrpc", "2.0",
        "method", "notifications/initialized");
    ASSERT_TRUE(maelys_mcp_channel_handle(channel_a, initialized) == MAELYS_MCP_OK);
    json_decref(initialized);

    /* Before MRTR is enabled, the call fails as an ordinary tool error. */
    json_t *params = json_pack("{s:s,s:{}}", "name", "test.confirm", "arguments");
    response = dispatch(channel_a, "tools/call", params);
    ASSERT_TRUE(json_is_true(json_object_get(json_object_get(response, "result"), "isError")));
    json_decref(response);

    ASSERT_TRUE(maelys_mcp_runtime_enable_module(core, MAELYS_MCP_MODULE_MRTR) == MAELYS_MCP_OK);

    /* MRTR is enabled, but this legacy client never declared elicitation:
     * refused explicitly, not silently allowed. */
    params = json_pack("{s:s,s:{}}", "name", "test.confirm", "arguments");
    response = dispatch(channel_a, "tools/call", params);
    ASSERT_TRUE(json_integer_value(json_object_get(json_object_get(response, "error"), "code")) == -32021);
    ASSERT_TRUE(json_is_object(json_object_get(json_object_get(json_object_get(response, "error"), "data"),
        "requiredCapabilities")));
    json_decref(response);
    ASSERT_TRUE(maelys_mcp_channel_destroy(channel_a) == MAELYS_MCP_OK);

    /* Channel B: legacy, elicitation declared at initialize. */
    maelys_mcp_channel_t *channel_b = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(core, NULL, &channel_b) == MAELYS_MCP_OK);
    json_t *init_b = json_pack("{s:s,s:{s:{}},s:{s:s,s:s}}",
        "protocolVersion", MAELYS_MCP_PROTOCOL_LEGACY, "capabilities", "elicitation",
        "clientInfo", "name", "legacy-with-caps", "version", "1");
    response = dispatch(channel_b, "initialize", init_b);
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);
    initialized = json_pack("{s:s,s:s}", "jsonrpc", "2.0",
        "method", "notifications/initialized");
    ASSERT_TRUE(maelys_mcp_channel_handle(channel_b, initialized) == MAELYS_MCP_OK);
    json_decref(initialized);

    params = json_pack("{s:s,s:{}}", "name", "test.confirm", "arguments");
    response = dispatch(channel_b, "tools/call", params);
    json_t *result = json_object_get(response, "result");
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(result, "resultType")), "input_required") == 0);
    ASSERT_TRUE(json_is_object(json_object_get(result, "inputRequests")));
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(result, "requestState")), "state-1") == 0);
    json_decref(response);
    /* The declared capability actually reached the provider over the wire,
     * not just mcp-runtime's own internal gate. */
    ASSERT_TRUE(state.saw_client_capabilities == 1);

    params = json_pack("{s:s,s:{},s:{s:{s:s}},s:s}",
        "name", "test.confirm", "arguments",
        "inputResponses", "confirmation", "action", "accept",
        "requestState", "state-1");
    response = dispatch(channel_b, "tools/call", params);
    result = json_object_get(response, "result");
    /*
     * Legacy channels never carry resultType/server-identity _meta on a
     * complete result — serialize_provider_result() gates that wire
     * convention on request->modern, by design (see docs/architecture.md:
     * "Modern final results carry resultType: 'complete', server identity
     * metadata..."). A legacy complete result is just the bare content
     * array, so assert on that instead of the modern-only envelope field.
     */
    ASSERT_TRUE(json_object_get(result, "resultType") == NULL);
    ASSERT_TRUE(json_array_size(json_object_get(result, "content")) >= 1u);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(
        json_array_get(json_object_get(result, "content"), 0), "text")), "confirmed") == 0);
    ASSERT_TRUE(state.saw_retry == 1);
    json_decref(response);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel_b) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(core) == MAELYS_MCP_OK);
    return 0;
}

/*
 * notifications/progress, and above all its ordering.
 *
 * The ordering assertion is the point. Progress is a JSON-RPC notification,
 * so routing it through the outbox's NOTIFICATION class looks right and is
 * wrong: that lane is the coalescible one for fanout unrelated to any
 * request, and select_next deliberately prefers responses over it, so the
 * call's final response overtakes the progress that preceded it. Over SSE
 * that is not just reordering - the final response terminates the stream, so
 * the progress would be dropped entirely. This test fails against that
 * mistake and passes only when request-scoped output shares the ordered lane
 * with the response it belongs to.
 */
typedef struct progress_state {
    int reports;
    int saw_reporter;
} progress_state_t;

static maelys_mcp_result_t progress_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *result,
    char **out_error) {
    progress_state_t *state = context;
    if (request->progress) state->saw_reporter = 1;
    /* Reported unconditionally: a NULL reporter is the documented way to say
     * "the client asked for none", so a provider never has to test first. */
    for (int step = 0; step <= 2; ++step) {
        if (maelys_mcp_provider_report_progress(request->progress,
                step * 50.0, 100.0, NULL) == MAELYS_MCP_OK) {
            if (request->progress) state->reports++;
        }
    }
    (void)out_error;
    result->content = json_pack("[{s:s,s:s}]", "type", "text", "text", "done");
    return result->content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static json_t *progress_params(json_t *token) {
    json_t *params = json_pack("{s:s,s:{},s:{s:s,s:{s:s,s:s},s:{}}}",
        "name", "test.progress", "arguments",
        "_meta",
        "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
        "io.modelcontextprotocol/clientInfo", "name", "test", "version", "1",
        "io.modelcontextprotocol/clientCapabilities");
    if (params && token) {
        (void)json_object_set_new(json_object_get(params, "_meta"),
            "progressToken", token);
    } else if (token) {
        json_decref(token);
    }
    return params;
}

static int test_progress_notifications(void) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "progress", .server_version = "1"
    };
    maelys_mcp_runtime_t *core = NULL;
    ASSERT_TRUE(maelys_mcp_runtime_create(&config, &core) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(core, MAELYS_MCP_MODULE_TOOLS) == MAELYS_MCP_OK);

    progress_state_t state = {0};
    maelys_mcp_tool_t tool = {
        .name = "test.progress",
        .title = "Reports progress",
        .description = "Emits three progress reports, then completes.",
        .effect = MAELYS_MCP_EFFECT_READ
    };
    tool.input_schema = json_pack("{s:s}", "type", "object");
    ASSERT_TRUE(tool.input_schema != NULL);
    maelys_mcp_provider_config_t provider_config = {
        .name = "progress-provider", .version = "1",
        .tools = &tool, .tool_count = 1,
        .call = progress_call, .context = &state
    };
    maelys_mcp_provider_t *provider = NULL;
    maelys_mcp_result_t created = maelys_mcp_provider_create(&provider_config, &provider);
    json_decref(tool.input_schema);
    ASSERT_TRUE(created == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(core, provider, NULL) == MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(core, NULL, &channel) == MAELYS_MCP_OK);

    /* With a token: three progress notifications, then the response. */
    json_t *owned = request("tools/call", progress_params(json_string("tok-1")));
    ASSERT_TRUE(maelys_mcp_channel_handle(channel, owned) == MAELYS_MCP_OK);
    json_decref(owned);
    ASSERT_TRUE(state.saw_reporter == 1);
    ASSERT_TRUE(state.reports == 3);

    for (int index = 0; index <= 2; ++index) {
        json_t *message = NULL;
        ASSERT_TRUE(maelys_mcp_channel_next(channel, 1000u, &message) == MAELYS_MCP_OK);
        /*
         * Ordering: every progress must arrive before the response. Checked
         * as "is this a notification at all" first, so that a regression
         * putting the response ahead of them fails with a readable assertion
         * instead of crashing in strcmp on a NULL method.
         */
        json_t *method = json_object_get(message, "method");
        ASSERT_TRUE(json_is_string(method));
        ASSERT_TRUE(strcmp(json_string_value(method), "notifications/progress") == 0);
        json_t *params = json_object_get(message, "params");
        ASSERT_TRUE(strcmp(json_string_value(
            json_object_get(params, "progressToken")), "tok-1") == 0);
        ASSERT_TRUE(json_number_value(json_object_get(params, "progress")) == index * 50.0);
        ASSERT_TRUE(json_number_value(json_object_get(params, "total")) == 100.0);
        /* Omitted, not sent empty, when the provider passes none. */
        ASSERT_TRUE(json_object_get(params, "message") == NULL);
        json_decref(message);
    }
    json_t *response = NULL;
    ASSERT_TRUE(maelys_mcp_channel_next(channel, 1000u, &response) == MAELYS_MCP_OK);
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);

    /* Without a token the provider gets no reporter, and nothing is emitted:
     * the very next message is the response itself. */
    state.reports = 0;
    state.saw_reporter = 0;
    owned = request("tools/call", progress_params(NULL));
    ASSERT_TRUE(maelys_mcp_channel_handle(channel, owned) == MAELYS_MCP_OK);
    json_decref(owned);
    ASSERT_TRUE(state.saw_reporter == 0);
    ASSERT_TRUE(state.reports == 0);
    ASSERT_TRUE(maelys_mcp_channel_next(channel, 1000u, &response) == MAELYS_MCP_OK);
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(core) == MAELYS_MCP_OK);
    return 0;
}

int main(void) {
    static const maelys_test_case_t tests[] = {
        {"content block validation", test_content_validation},
        {"module registry, rich content, and MRTR", test_modules_rich_content_and_mrtr},
        {"legacy channel MRTR capability negotiation", test_legacy_mrtr_capability_negotiation},
        {"progress notifications and their ordering", test_progress_notifications}
    };
    return maelys_run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
