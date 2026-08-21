#include "maelys/mcp.h"
#include "tests/test_support.h"

#include <stdlib.h>
#include <string.h>

static maelys_mcp_runtime_t *new_runtime(maelys_mcp_channel_t **out_channel) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "protocol-test",
        .server_version = "1.0",
        .max_providers = 2
    };
    maelys_mcp_runtime_t *runtime = NULL;
    if (maelys_mcp_runtime_create(&config, &runtime) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_TOOLS) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_MRTR) != MAELYS_MCP_OK) {
        if (runtime) {
            maelys_mcp_result_t destroy_status = maelys_mcp_runtime_destroy(runtime);
            (void)destroy_status;
        }
        return NULL;
    }
    if (out_channel &&
        maelys_mcp_channel_create(runtime, NULL, out_channel) != MAELYS_MCP_OK) {
        maelys_mcp_result_t destroy_status = maelys_mcp_runtime_destroy(runtime);
        (void)destroy_status;
        return NULL;
    }
    return runtime;
}

/*
 * A channel whose era mask is chosen at creation, which since ABI 4 is the
 * only way to choose it: maelys_mcp_channel_config_t::protocol_eras replaced
 * maelys_mcp_channel_set_protocol_eras, so every mask below is fixed before
 * the channel can carry a single frame.
 */
static maelys_mcp_channel_t *new_era_channel(
    maelys_mcp_runtime_t *runtime,
    unsigned int eras) {
    maelys_mcp_channel_config_t config = { .protocol_eras = eras };
    maelys_mcp_channel_t *channel = NULL;
    return maelys_mcp_channel_create(runtime, &config, &channel) == MAELYS_MCP_OK ?
        channel : NULL;
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

static json_t *dispatch(maelys_mcp_channel_t *channel, json_t *owned_request) {
    maelys_mcp_result_t status = maelys_mcp_channel_handle(channel, owned_request);
    json_decref(owned_request);
    if (status != MAELYS_MCP_OK) return NULL;
    json_t *response = NULL;
    if (maelys_mcp_channel_next(channel, 1000u, &response) != MAELYS_MCP_OK) {
        return NULL;
    }
    return response;
}

static int cleanup(
    maelys_mcp_runtime_t *runtime,
    maelys_mcp_channel_t *channel) {
    return maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK &&
        maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK;
}

static int test_invalid_envelopes_and_ids(void) {
    maelys_mcp_channel_t *channel = NULL;
    maelys_mcp_runtime_t *runtime = new_runtime(&channel);
    ASSERT_TRUE(runtime != NULL);
    json_t *array_request = json_array();
    json_t *response = dispatch(channel, array_request);
    ASSERT_TRUE(error_code(response) == -32600);
    json_decref(response);
    json_t *invalid[] = {
        json_pack("{s:s,s:i,s:s}", "jsonrpc", "1.0", "id", 1, "method", "x"),
        json_pack("{s:s,s:i,s:i}", "jsonrpc", "2.0", "id", 1, "method", 2),
        json_pack("{s:s,s:b,s:s}", "jsonrpc", "2.0", "id", 1, "method", "x"),
        json_pack("{s:s,s:n,s:s}", "jsonrpc", "2.0", "id", "method", "x")
    };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        ASSERT_TRUE(invalid[index] != NULL);
        response = dispatch(channel, invalid[index]);
        ASSERT_TRUE(error_code(response) == -32600);
        if (index >= 2) ASSERT_TRUE(json_is_null(json_object_get(response, "id")));
        json_decref(response);
    }
    const char hostile_method[] = {'t', 'o', 'o', 'l', 's', '/', 'l', 'i', 's', 't', '\0', 'x'};
    json_t *nul_method = json_pack("{s:s,s:i,s:o,s:o}",
        "jsonrpc", "2.0", "id", 7, "method",
        json_stringn(hostile_method, sizeof(hostile_method)), "params", modern_params());
    ASSERT_TRUE(nul_method != NULL);
    response = dispatch(channel, nul_method);
    ASSERT_TRUE(error_code(response) == -32600);
    json_decref(response);
    ASSERT_TRUE(cleanup(runtime, channel));
    return 0;
}

static int test_notifications_have_no_response_or_side_effect(void) {
    maelys_mcp_channel_t *channel = NULL;
    maelys_mcp_runtime_t *runtime = new_runtime(&channel);
    ASSERT_TRUE(runtime != NULL);
    json_t *notification = request(NULL, "tools/call", modern_params());
    ASSERT_TRUE(notification != NULL);
    ASSERT_TRUE(maelys_mcp_channel_handle(channel, notification) == MAELYS_MCP_OK);
    json_decref(notification);
    ASSERT_TRUE(cleanup(runtime, channel));
    return 0;
}

static int test_modern_metadata_contract(void) {
    maelys_mcp_channel_t *channel = NULL;
    maelys_mcp_runtime_t *runtime = new_runtime(&channel);
    ASSERT_TRUE(runtime != NULL);
    json_t *response = dispatch(channel,
        request(json_integer(1), "server/discover", json_object()));
    ASSERT_TRUE(error_code(response) == -32602);
    json_decref(response);

    json_t *params = modern_params();
    json_object_set_new(json_object_get(params, "_meta"),
        "io.modelcontextprotocol/clientInfo", json_pack("{s:s}", "name", "missing-version"));
    response = dispatch(channel,
        request(json_string("abc"), "tools/list", params));
    ASSERT_TRUE(error_code(response) == -32602);
    ASSERT_TRUE(json_is_string(json_object_get(response, "id")));
    json_decref(response);

    params = modern_params();
    json_object_set_new(json_object_get(params, "_meta"),
        "io.modelcontextprotocol/protocolVersion", json_string("2099-01-01"));
    response = dispatch(channel,
        request(json_integer(3), "tools/list", params));
    ASSERT_TRUE(error_code(response) == -32022);
    json_decref(response);
    ASSERT_TRUE(cleanup(runtime, channel));
    return 0;
}

static int test_legacy_lifecycle_and_unknown_method(void) {
    maelys_mcp_channel_t *channel = NULL;
    maelys_mcp_runtime_t *runtime = new_runtime(&channel);
    ASSERT_TRUE(runtime != NULL);
    json_t *response = dispatch(channel,
        request(json_integer(1), "tools/list", json_object()));
    ASSERT_TRUE(error_code(response) == -32002);
    json_decref(response);

    json_t *bad_init = json_pack("{s:s}", "protocolVersion", "old");
    response = dispatch(channel,
        request(json_integer(2), "initialize", bad_init));
    ASSERT_TRUE(error_code(response) == -32602);
    json_decref(response);

    json_t *init = json_pack("{s:s,s:{},s:{s:s,s:s}}",
        "protocolVersion", MAELYS_MCP_PROTOCOL_LEGACY, "capabilities",
        "clientInfo", "name", "client", "version", "1");
    response = dispatch(channel,
        request(json_integer(3), "initialize", init));
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);
    response = dispatch(channel,
        request(json_integer(4), "initialize", json_object()));
    ASSERT_TRUE(error_code(response) == -32600);
    json_decref(response);
    json_t *initialized = request(NULL, "notifications/initialized", json_object());
    ASSERT_TRUE(maelys_mcp_channel_handle(channel, initialized) == MAELYS_MCP_OK);
    json_decref(initialized);
    response = dispatch(channel,
        request(json_integer(5), "unknown/method", json_object()));
    ASSERT_TRUE(error_code(response) == -32601);
    json_decref(response);
    ASSERT_TRUE(cleanup(runtime, channel));
    return 0;
}

static int test_ping_answers_in_every_state(void) {
    maelys_mcp_channel_t *channel = NULL;
    maelys_mcp_runtime_t *runtime = new_runtime(&channel);
    ASSERT_TRUE(runtime != NULL);
    /*
     * Before any initialize: a liveness probe must not be refused for
     * lifecycle reasons - the protocol allows a ping at any time, and this is
     * the state where being able to answer one matters most.
     */
    json_t *response = dispatch(channel,
        request(json_integer(1), "ping", json_object()));
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(json_object_get(response, "error") == NULL);
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    ASSERT_TRUE(json_object_size(json_object_get(response, "result")) == 0u);
    json_decref(response);
    /* Modern, with full per-request metadata. */
    response = dispatch(channel,
        request(json_integer(2), "ping", modern_params()));
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(json_object_get(response, "error") == NULL);
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);
    /* Legacy, after the ordinary handshake. */
    json_t *init = json_pack("{s:s,s:{},s:{s:s,s:s}}",
        "protocolVersion", MAELYS_MCP_PROTOCOL_LEGACY, "capabilities",
        "clientInfo", "name", "client", "version", "1");
    response = dispatch(channel, request(json_integer(3), "initialize", init));
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);
    json_t *initialized = request(NULL, "notifications/initialized", json_object());
    ASSERT_TRUE(maelys_mcp_channel_handle(channel, initialized) == MAELYS_MCP_OK);
    json_decref(initialized);
    response = dispatch(channel,
        request(json_integer(4), "ping", json_object()));
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(json_object_get(response, "error") == NULL);
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);
    /* Malformed modern metadata on a ping is still rejected: answering in
     * every lifecycle state does not mean skipping request validation. */
    json_t *bad_meta = json_pack("{s:{s:s}}", "_meta",
        "io.modelcontextprotocol/protocolVersion", "2099-01-01");
    response = dispatch(channel, request(json_integer(5), "ping", bad_meta));
    ASSERT_TRUE(json_object_get(response, "error") != NULL);
    json_decref(response);
    ASSERT_TRUE(cleanup(runtime, channel));
    return 0;
}

static maelys_mcp_result_t invalid_output_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    (void)context; (void)request; (void)out_error;
    out_result->structured_content = json_pack("{s:s}", "ok", "not-a-boolean");
    return out_result->structured_content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static int test_legacy_version_negotiation(void) {
    static const char *const versions[] = {
        "2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25",
    };
    for (size_t index = 0; index < sizeof(versions) / sizeof(versions[0]); ++index) {
        maelys_mcp_channel_t *channel = NULL;
        maelys_mcp_runtime_t *runtime = new_runtime(&channel);
        ASSERT_TRUE(runtime != NULL);
        json_t *init = json_pack("{s:s,s:{},s:{s:s,s:s}}",
            "protocolVersion", versions[index], "capabilities",
            "clientInfo", "name", "codex", "version", "1");
        json_t *response = dispatch(channel,
            request(json_integer(1), "initialize", init));
        json_t *result = json_object_get(response, "result");
        json_t *negotiated = json_is_object(result) ?
            json_object_get(result, "protocolVersion") : NULL;
        /* MCP negotiation: a supported requested version is echoed back. */
        ASSERT_TRUE(json_is_string(negotiated) &&
            strcmp(json_string_value(negotiated), versions[index]) == 0);
        json_decref(response);
        ASSERT_TRUE(cleanup(runtime, channel));
    }
    maelys_mcp_channel_t *channel = NULL;
    maelys_mcp_runtime_t *runtime = new_runtime(&channel);
    ASSERT_TRUE(runtime != NULL);
    json_t *bad = json_pack("{s:s}", "protocolVersion", "2020-01-01");
    json_t *response = dispatch(channel,
        request(json_integer(1), "initialize", bad));
    ASSERT_TRUE(error_code(response) == -32602);
    json_decref(response);
    ASSERT_TRUE(cleanup(runtime, channel));
    return 0;
}

static int test_legacy_channel_ignores_opaque_request_meta(void) {
    maelys_mcp_channel_t *channel = NULL;
    maelys_mcp_runtime_t *runtime = new_runtime(&channel);
    ASSERT_TRUE(runtime != NULL);
    json_t *init = json_pack("{s:s,s:{},s:{s:s,s:s}}",
        "protocolVersion", MAELYS_MCP_PROTOCOL_LEGACY, "capabilities",
        "clientInfo", "name", "hermes", "version", "1");
    json_t *response = dispatch(channel,
        request(json_integer(1), "initialize", init));
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);
    json_t *initialized = request(NULL, "notifications/initialized", json_object());
    ASSERT_TRUE(maelys_mcp_channel_handle(channel, initialized) == MAELYS_MCP_OK);
    json_decref(initialized);

    /*
     * A legacy-initialized channel making an ordinary request with an opaque,
     * application-defined `_meta` (e.g. a progress token) must not be forced
     * through modern protocolVersion/clientCapabilities validation: the
     * protocol was already pinned by initialize. This reproduces the failure
     * a legacy client (e.g. Hermes) hit on tools/list with such a `_meta`.
     */
    json_t *params = json_pack("{s:{s:s}}", "_meta", "progressToken", "abc");
    response = dispatch(channel,
        request(json_integer(2), "tools/list", params));
    ASSERT_TRUE(error_code(response) != -32602);
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);
    ASSERT_TRUE(cleanup(runtime, channel));
    return 0;
}

static int test_output_schema_failure_is_fail_closed(void) {
    maelys_mcp_channel_t *channel = NULL;
    maelys_mcp_runtime_t *runtime = new_runtime(NULL);
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
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);
    json_t *params = modern_params();
    json_object_set_new(params, "name", json_string("test.invalid-output"));
    json_object_set_new(params, "arguments", json_object());
    json_t *response = dispatch(channel,
        request(json_integer(1), "tools/call", params));
    json_t *result = json_object_get(response, "result");
    ASSERT_TRUE(json_is_true(json_object_get(result, "isError")));
    ASSERT_TRUE(json_object_get(result, "structuredContent") == NULL);
    json_decref(response);
    ASSERT_TRUE(cleanup(runtime, channel));
    return 0;
}

/* The `supportedVersions` array server/discover announced, as a sorted-by-
 * appearance list of strings, or NULL if the response was not a result. */
static json_t *announced_versions(maelys_mcp_channel_t *channel) {
    json_t *response = dispatch(channel,
        request(json_integer(1), "server/discover", modern_params()));
    if (!response) return NULL;
    json_t *versions = json_incref(json_object_get(
        json_object_get(response, "result"), "supportedVersions"));
    json_decref(response);
    return versions;
}

static int array_holds(json_t *array, const char *value) {
    size_t index = 0;
    json_t *entry = NULL;
    json_array_foreach(array, index, entry) {
        if (json_is_string(entry) && strcmp(json_string_value(entry), value) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * The default has to be proven, not assumed: zero means MAELYS_MCP_ERA_ALL,
 * which is what keeps a config that predates the field - a NULL one, or a
 * zero-initialized one that never mentions eras - behaving exactly as it did
 * before ABI 4. Both spellings are checked, because the field is what a
 * memset-and-fill embedder passes without knowing it exists.
 */
static int test_default_channel_announces_both_eras(void) {
    maelys_mcp_channel_t *channel = NULL;
    maelys_mcp_runtime_t *runtime = new_runtime(&channel);
    ASSERT_TRUE(runtime != NULL);
    json_t *versions = announced_versions(channel);
    ASSERT_TRUE(json_array_size(versions) == 2);
    ASSERT_TRUE(array_holds(versions, MAELYS_MCP_PROTOCOL_MODERN));
    ASSERT_TRUE(array_holds(versions, MAELYS_MCP_PROTOCOL_LEGACY));
    json_decref(versions);

    /* An explicit zero in the field is the same channel as no config at all. */
    maelys_mcp_channel_t *zeroed = new_era_channel(runtime, 0u);
    ASSERT_TRUE(zeroed != NULL);
    versions = announced_versions(zeroed);
    ASSERT_TRUE(json_array_size(versions) == 2);
    ASSERT_TRUE(array_holds(versions, MAELYS_MCP_PROTOCOL_MODERN));
    ASSERT_TRUE(array_holds(versions, MAELYS_MCP_PROTOCOL_LEGACY));
    json_decref(versions);
    ASSERT_TRUE(maelys_mcp_channel_destroy(zeroed) == MAELYS_MCP_OK);
    /* And the legacy handshake still works on it. */
    json_t *init = json_pack("{s:s,s:{},s:{s:s,s:s}}",
        "protocolVersion", MAELYS_MCP_PROTOCOL_LEGACY, "capabilities",
        "clientInfo", "name", "codex", "version", "1");
    json_t *response = dispatch(channel, request(json_integer(2), "initialize", init));
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);
    ASSERT_TRUE(cleanup(runtime, channel));
    return 0;
}

static int test_modern_only_channel_announces_and_refuses(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(NULL);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_channel_t *channel = new_era_channel(runtime,
        MAELYS_MCP_ERA_MODERN);
    ASSERT_TRUE(channel != NULL);
    json_t *versions = announced_versions(channel);
    ASSERT_TRUE(json_array_size(versions) == 1);
    ASSERT_TRUE(array_holds(versions, MAELYS_MCP_PROTOCOL_MODERN));
    json_decref(versions);

    /* initialize is refused as an invalid request, not as bad params: the
     * params are fine, the handshake simply does not exist here. */
    json_t *init = json_pack("{s:s,s:{},s:{s:s,s:s}}",
        "protocolVersion", MAELYS_MCP_PROTOCOL_LEGACY, "capabilities",
        "clientInfo", "name", "codex", "version", "1");
    json_t *response = dispatch(channel,
        request(json_integer(2), "initialize", init));
    ASSERT_TRUE(error_code(response) == -32600);
    json_decref(response);

    /* And nothing was recorded by that refusal, so an ordinary uninitialized
     * request still gets the not-initialized answer rather than a result. */
    response = dispatch(channel, request(json_integer(3), "tools/list", NULL));
    ASSERT_TRUE(error_code(response) == -32002);
    json_decref(response);

    /* Modern negotiation is untouched. */
    response = dispatch(channel,
        request(json_integer(4), "tools/list", modern_params()));
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);
    ASSERT_TRUE(cleanup(runtime, channel));
    return 0;
}

static int test_legacy_only_channel_refuses_modern_negotiation(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(NULL);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_channel_t *channel = new_era_channel(runtime,
        MAELYS_MCP_ERA_LEGACY);
    ASSERT_TRUE(channel != NULL);

    /* server/discover is a modern entry point, so it is refused with the
     * version error and the error names what this channel does serve. */
    json_t *response = dispatch(channel,
        request(json_integer(1), "server/discover", modern_params()));
    ASSERT_TRUE(error_code(response) == -32022);
    json_t *supported = json_object_get(
        json_object_get(json_object_get(response, "error"), "data"), "supported");
    ASSERT_TRUE(json_array_size(supported) == 1);
    ASSERT_TRUE(array_holds(supported, MAELYS_MCP_PROTOCOL_LEGACY));
    json_decref(response);

    /* The legacy handshake still works, and a legacy request after it works. */
    json_t *init = json_pack("{s:s,s:{},s:{s:s,s:s}}",
        "protocolVersion", MAELYS_MCP_PROTOCOL_LEGACY, "capabilities",
        "clientInfo", "name", "codex", "version", "1");
    response = dispatch(channel, request(json_integer(2), "initialize", init));
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);
    json_t *initialized = request(NULL, "notifications/initialized", json_object());
    ASSERT_TRUE(maelys_mcp_channel_handle(channel, initialized) == MAELYS_MCP_OK);
    json_decref(initialized);
    response = dispatch(channel, request(json_integer(3), "tools/list", NULL));
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);

    /* But an attempt to renegotiate into the modern era is refused. */
    response = dispatch(channel,
        request(json_integer(4), "tools/list", modern_params()));
    ASSERT_TRUE(error_code(response) == -32022);
    json_decref(response);
    ASSERT_TRUE(cleanup(runtime, channel));
    return 0;
}

/*
 * An impossible mask is refused at creation, and refused loudly: zero is the
 * one value that means "both eras", so the mistake the old setter caught by
 * rejecting zero is now caught by rejecting bits that name no era at all. A
 * refused config produces no channel, which is the property that stops a
 * caller from proceeding with a channel it never asked for.
 */
static int test_protocol_era_config_rejects_impossible_masks(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(NULL);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_channel_config_t config = { .protocol_eras = 1u << 7 };
    maelys_mcp_channel_t *channel = (maelys_mcp_channel_t *)0x1;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, &config, &channel) ==
        MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(channel == NULL);
    config.protocol_eras = MAELYS_MCP_ERA_ALL | 1u << 7;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, &config, &channel) ==
        MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(channel == NULL);

    /* And a refusal costs the runtime nothing: the next channel still works. */
    maelys_mcp_channel_t *good = new_era_channel(runtime, MAELYS_MCP_ERA_ALL);
    ASSERT_TRUE(good != NULL);
    json_t *versions = announced_versions(good);
    ASSERT_TRUE(json_array_size(versions) == 2);
    json_decref(versions);
    ASSERT_TRUE(cleanup(runtime, good));
    return 0;
}

/*
 * The rule that died with the setter, asserted from the other side: a
 * legacy-negotiated channel keeps serving legacy for the rest of its life,
 * because nothing can narrow it mid-session any more. The old
 * ERR_STATE-on-withdrawal case exists here as a channel that simply cannot be
 * put into that position.
 */
static int test_negotiated_era_cannot_be_withdrawn_mid_session(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(NULL);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_channel_t *channel = new_era_channel(runtime, MAELYS_MCP_ERA_ALL);
    ASSERT_TRUE(channel != NULL);
    json_t *init = json_pack("{s:s,s:{},s:{s:s,s:s}}",
        "protocolVersion", MAELYS_MCP_PROTOCOL_LEGACY, "capabilities",
        "clientInfo", "name", "codex", "version", "1");
    json_t *response = dispatch(channel,
        request(json_integer(1), "initialize", init));
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);
    json_t *initialized = request(NULL, "notifications/initialized", json_object());
    ASSERT_TRUE(maelys_mcp_channel_handle(channel, initialized) == MAELYS_MCP_OK);
    json_decref(initialized);
    response = dispatch(channel, request(json_integer(2), "tools/list", NULL));
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);
    json_t *versions = announced_versions(channel);
    ASSERT_TRUE(json_array_size(versions) == 2);
    json_decref(versions);
    ASSERT_TRUE(cleanup(runtime, channel));
    return 0;
}

int main(void) {
    static const maelys_test_case_t tests[] = {
        {"default channel announces both eras", test_default_channel_announces_both_eras},
        {"modern-only channel announces one era and refuses initialize", test_modern_only_channel_announces_and_refuses},
        {"legacy-only channel refuses modern negotiation", test_legacy_only_channel_refuses_modern_negotiation},
        {"protocol era config rejects impossible masks", test_protocol_era_config_rejects_impossible_masks},
        {"a negotiated era cannot be withdrawn mid-session", test_negotiated_era_cannot_be_withdrawn_mid_session},
        {"invalid JSON-RPC envelopes and id types", test_invalid_envelopes_and_ids},
        {"notifications do not produce responses", test_notifications_have_no_response_or_side_effect},
        {"modern metadata and version contract", test_modern_metadata_contract},
        {"legacy initialization lifecycle and unknown method", test_legacy_lifecycle_and_unknown_method},
        {"legacy protocol version negotiation", test_legacy_version_negotiation},
        {"legacy channel ignores opaque request meta", test_legacy_channel_ignores_opaque_request_meta},
        {"ping answers in every state", test_ping_answers_in_every_state},
        {"invalid provider output is fail-closed", test_output_schema_failure_is_fail_closed}
    };
    return maelys_run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
