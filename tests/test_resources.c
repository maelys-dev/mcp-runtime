#include "maelys/mcp.h"
#include "tests/test_support.h"

#include <stdlib.h>
#include <string.h>

typedef struct resource_state {
    int reads;
    int saw_client_capabilities;
} resource_state_t;

static maelys_mcp_result_t read_resource(
    void *context,
    const maelys_mcp_resource_request_t *request,
    maelys_mcp_resource_result_t *result,
    char **out_error) {
    resource_state_t *state = context;
    state->reads++;
    if (strcmp(request->uri, "hermes://repository/course.mdx") == 0) {
        result->contents = json_pack("[{s:s,s:s,s:s}]",
            "uri", request->uri, "mimeType", "text/markdown", "text", "# Course");
        return result->contents ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
    }
    if (strncmp(request->uri, "hermes://repository/assets/", 27u) == 0) {
        result->contents = json_pack("[{s:s,s:s,s:s}]",
            "uri", request->uri, "mimeType", "application/octet-stream", "blob", "aGVsbG8=");
        return result->contents ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
    }
    if (strcmp(request->uri, "hermes://repository/gated.mdx") == 0) {
        /*
         * Wire-level check, not just mcp-runtime's internal gate: a real
         * out-of-process provider only ever sees what travels over
         * maelys-provider/3, so the fix must put the legacy-declared
         * capability where the provider can actually read it.
         */
        if (json_is_object(request->client_capabilities) &&
            json_is_object(json_object_get(request->client_capabilities, "roots"))) {
            state->saw_client_capabilities = 1;
        }
        if (!request->input_responses) {
            result->type = MAELYS_MCP_RESOURCE_RESULT_INPUT_REQUIRED;
            result->input_requests = json_pack("{s:{s:s,s:{}}}",
                "hermes_roots", "method", "roots/list", "params");
            result->request_state = json_string("state-1");
            return result->input_requests && result->request_state ?
                MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
        }
        result->contents = json_pack("[{s:s,s:s,s:s}]",
            "uri", request->uri, "mimeType", "text/markdown", "text", "# Gated");
        return result->contents ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
    }
    if (out_error) *out_error = strdup("resource not found");
    return MAELYS_MCP_ERR_NOT_FOUND;
}

static json_t *modern_params(void) {
    return json_pack("{s:{s:s,s:{s:s,s:s},s:{}}}",
        "_meta",
        "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
        "io.modelcontextprotocol/clientInfo", "name", "resource-test", "version", "1",
        "io.modelcontextprotocol/clientCapabilities");
}

static json_t *dispatch(
    maelys_mcp_channel_t *channel,
    const char *method,
    json_t *params) {
    json_t *request = json_pack("{s:s,s:i,s:s,s:o}",
        "jsonrpc", "2.0", "id", 1, "method", method, "params", params);
    maelys_mcp_result_t status = maelys_mcp_channel_handle(channel, request);
    json_decref(request);
    if (status != MAELYS_MCP_OK) return NULL;
    json_t *response = NULL;
    if (maelys_mcp_channel_next(channel, 1000u, &response) != MAELYS_MCP_OK) {
        return NULL;
    }
    return response;
}

static maelys_mcp_provider_t *provider(resource_state_t *state) {
    static const maelys_mcp_resource_t resources[] = {{
        .uri = "HERMES://repository/course.mdx",
        .name = "Course",
        .title = "Course source",
        .description = "An editorial source",
        .mime_type = "text/markdown",
        .has_size = 1,
        .size = 8
    }};
    static const maelys_mcp_resource_template_t templates[] = {{
        .uri_template = "hermes://repository/assets/{path}",
        .name = "Assets",
        .description = "Repository assets",
        .mime_type = "application/octet-stream"
    }};
    maelys_mcp_provider_config_t config = {
        .name = "resources", .version = "1",
        .resources = resources, .resource_count = 1,
        .resource_templates = templates, .resource_template_count = 1,
        .read_resource = read_resource, .context = state
    };
    maelys_mcp_provider_t *value = NULL;
    return maelys_mcp_provider_create(&config, &value) == MAELYS_MCP_OK ? value : NULL;
}

static int test_uri_facade(void) {
    maelys_uri_options_t options = {.max_bytes = 256u, .require_scheme = 1};
    maelys_uri_t *uri = NULL;
    char *error = NULL;
    const char input[] = "HERMES://repository/a/../course.mdx";
    ASSERT_TRUE(maelys_uri_parse(input, sizeof(input) - 1u,
        &options, &uri, &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(strcmp(maelys_uri_scheme(uri), "hermes") == 0);
    ASSERT_TRUE(strcmp(maelys_uri_canonical(uri), "hermes://repository/course.mdx") == 0);
    maelys_uri_destroy(uri);
    uri = NULL;
    ASSERT_TRUE(maelys_uri_parse("file:///tmp/%00.png", 19u,
        &options, &uri, &error) == MAELYS_MCP_ERR_PROTOCOL);
    free(error);
    error = NULL;
    ASSERT_TRUE(maelys_uri_template_validate("hermes://repo/{path}", 20u,
        256u, &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_uri_template_validate("hermes://repo/{path", 19u,
        256u, &error) == MAELYS_MCP_ERR_PROTOCOL);
    free(error);
    return 0;
}

static int test_resource_module(void) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "resources", .server_version = "0.4.0"
    };
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(maelys_mcp_runtime_create(&config, &runtime) == MAELYS_MCP_OK);
    resource_state_t state = {0};
    maelys_mcp_provider_t *value = provider(&state);
    ASSERT_TRUE(value != NULL);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, value, NULL) == MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(runtime,
        MAELYS_MCP_MODULE_RESOURCES) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, value, NULL) == MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

    json_t *response = dispatch(channel, "resources/list", modern_params());
    json_t *result = json_object_get(response, "result");
    json_t *listed = json_array_get(json_object_get(result, "resources"), 0);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(listed, "uri")),
        "hermes://repository/course.mdx") == 0);
    ASSERT_TRUE(json_integer_value(json_object_get(listed, "size")) == 8);
    ASSERT_TRUE(json_integer_value(json_object_get(result, "ttlMs")) == 0);
    json_decref(response);

    response = dispatch(channel, "resources/templates/list", modern_params());
    result = json_object_get(response, "result");
    ASSERT_TRUE(json_array_size(json_object_get(result, "resourceTemplates")) == 1u);
    json_decref(response);

    json_t *params = modern_params();
    json_object_set_new(params, "uri", json_string("HERMES://repository/course.mdx"));
    response = dispatch(channel, "resources/read", params);
    result = json_object_get(response, "result");
    json_t *content = json_array_get(json_object_get(result, "contents"), 0);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(content, "text")), "# Course") == 0);
    json_decref(response);

    params = modern_params();
    json_object_set_new(params, "uri", json_string("hermes://repository/assets/logo.bin"));
    response = dispatch(channel, "resources/read", params);
    result = json_object_get(response, "result");
    content = json_array_get(json_object_get(result, "contents"), 0);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(content, "blob")), "aGVsbG8=") == 0);
    ASSERT_TRUE(state.reads == 2);
    json_decref(response);

    params = modern_params();
    json_object_set_new(params, "uri", json_string("file:///tmp/%00.png"));
    response = dispatch(channel, "resources/read", params);
    ASSERT_TRUE(json_integer_value(json_object_get(json_object_get(response, "error"), "code")) == -32602);
    json_decref(response);
    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

static int test_legacy_resource_mrtr_capability_negotiation(void) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "resources", .server_version = "0.4.0"
    };
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(maelys_mcp_runtime_create(&config, &runtime) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(runtime,
        MAELYS_MCP_MODULE_RESOURCES) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(runtime,
        MAELYS_MCP_MODULE_MRTR) == MAELYS_MCP_OK);
    resource_state_t state = {0};
    maelys_mcp_provider_t *value = provider(&state);
    ASSERT_TRUE(value != NULL);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, value, NULL) == MAELYS_MCP_OK);

    /* Legacy channel, elicitation/roots declared at initialize. */
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);
    json_t *init = json_pack("{s:s,s:{s:{}},s:{s:s,s:s}}",
        "protocolVersion", MAELYS_MCP_PROTOCOL_LEGACY, "capabilities", "roots",
        "clientInfo", "name", "legacy-resource-reader", "version", "1");
    json_t *response = dispatch(channel, "initialize", init);
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);
    json_t *initialized = json_pack("{s:s,s:s}", "jsonrpc", "2.0",
        "method", "notifications/initialized");
    ASSERT_TRUE(maelys_mcp_channel_handle(channel, initialized) == MAELYS_MCP_OK);
    json_decref(initialized);

    json_t *params = json_pack("{s:s}", "uri", "hermes://repository/gated.mdx");
    response = dispatch(channel, "resources/read", params);
    json_t *result = json_object_get(response, "result");
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(result, "resultType")),
        "input_required") == 0);
    json_t *hermes_roots = json_object_get(json_object_get(result, "inputRequests"),
        "hermes_roots");
    ASSERT_TRUE(json_is_object(hermes_roots));
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(hermes_roots, "method")),
        "roots/list") == 0);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(result, "requestState")),
        "state-1") == 0);
    json_decref(response);
    /* The declared capability actually reached the provider over the wire. */
    ASSERT_TRUE(state.saw_client_capabilities == 1);

    params = json_pack("{s:s,s:{s:{s:[]}},s:s}",
        "uri", "hermes://repository/gated.mdx",
        "inputResponses", "hermes_roots", "roots",
        "requestState", "state-1");
    response = dispatch(channel, "resources/read", params);
    result = json_object_get(response, "result");
    /*
     * Legacy channels never carry resultType/server-identity _meta on a
     * complete result - that wire convention is gated on request->modern by
     * design (see docs/architecture.md). A legacy complete result is just
     * the bare contents array.
     */
    ASSERT_TRUE(json_object_get(result, "resultType") == NULL);
    json_t *content = json_array_get(json_object_get(result, "contents"), 0);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(content, "text")), "# Gated") == 0);
    json_decref(response);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

static int test_resource_content_validation(void) {
    char *error = NULL;
    json_t *valid = json_pack("[{s:s,s:s,s:s}]",
        "uri", "test://resource", "mimeType", "text/plain", "text", "ok");
    ASSERT_TRUE(maelys_mcp_validate_resource_contents(valid, 1024u, &error) == MAELYS_MCP_OK);
    json_decref(valid);
    const char hostile[] = {'a', 'G', 'V', 's', '\0', '='};
    json_t *invalid = json_pack("[{s:s,s:o}]", "uri", "test://resource",
        "blob", json_stringn(hostile, sizeof(hostile)));
    ASSERT_TRUE(maelys_mcp_validate_resource_contents(invalid,
        1024u, &error) == MAELYS_MCP_ERR_PROTOCOL);
    free(error);
    json_decref(invalid);
    return 0;
}

int main(void) {
    static const maelys_test_case_t tests[] = {
        {"opaque URI facade and templates", test_uri_facade},
        {"resources list, templates and read", test_resource_module},
        {"legacy channel resources/read MRTR capability negotiation",
            test_legacy_resource_mrtr_capability_negotiation},
        {"resource content validation", test_resource_content_validation}
    };
    return maelys_run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
