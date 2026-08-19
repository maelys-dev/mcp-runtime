#include "maelys/mcp.h"
/*
 * Whitebox on purpose: the pinned-snapshot rejection and the fast-fail after
 * an upstream dies happen inside the provider callback, below the registry a
 * runtime would resolve names against, so they cannot be reached through
 * maelys_mcp_channel_handle at all.
 */
#include "src/internal/internal.h"

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

typedef struct harness {
    maelys_mcp_runtime_t *runtime;
    maelys_mcp_channel_t *channel;
} harness_t;

static long long milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (long long)now.tv_sec * 1000LL + (long long)now.tv_nsec / 1000000LL;
}

static json_t *next_message(maelys_mcp_channel_t *channel) {
    json_t *message = NULL;
    return maelys_mcp_channel_next(channel, 5000u, &message) == MAELYS_MCP_OK ?
        message : NULL;
}

static json_t *dispatch(maelys_mcp_channel_t *channel, json_t *request) {
    maelys_mcp_result_t status = maelys_mcp_channel_handle(channel, request);
    json_decref(request);
    return status == MAELYS_MCP_OK ? next_message(channel) : NULL;
}

static json_t *modern_meta(void) {
    return json_pack("{s:s,s:{s:s,s:s},s:{}}",
        "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
        "io.modelcontextprotocol/clientInfo", "name", "proxy-test", "version", "1",
        "io.modelcontextprotocol/clientCapabilities");
}

static json_t *request_with(int id, const char *method, json_t *params) {
    json_t *meta = modern_meta();
    if (!meta || !params || json_object_set_new(params, "_meta", meta) != 0) return NULL;
    return json_pack("{s:s,s:i,s:s,s:o}",
        "jsonrpc", "2.0", "id", id, "method", method, "params", params);
}

static int harness_open(harness_t *harness, maelys_mcp_provider_t *provider) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "proxy-test", .server_version = "1.0"
    };
    if (maelys_mcp_runtime_create(&config, &harness->runtime) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_enable_module(harness->runtime,
            MAELYS_MCP_MODULE_TOOLS) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_add_provider(harness->runtime, provider, NULL) != MAELYS_MCP_OK) {
        return -1;
    }
    maelys_mcp_channel_config_t channel_config = {
        .max_messages = 16u, .max_bytes = 65536u, .response_burst = 2u,
        .admission_timeout_ms = 5000u, .close_timeout_ms = 5000u
    };
    return maelys_mcp_channel_create(harness->runtime, &channel_config,
        &harness->channel) == MAELYS_MCP_OK ? 0 : -1;
}

static int harness_close(harness_t *harness) {
    return maelys_mcp_channel_destroy(harness->channel) == MAELYS_MCP_OK &&
        maelys_mcp_runtime_destroy(harness->runtime) == MAELYS_MCP_OK ? 0 : -1;
}

/* Every assertion below runs against upstream-shaped JSON, so a missing field
 * has to read as a failed assertion rather than a crash on json_string_value. */
static int string_is(json_t *value, const char *expected) {
    return json_is_string(value) && strcmp(json_string_value(value), expected) == 0;
}

static json_t *find_tool(json_t *response, const char *name) {
    json_t *tools = json_object_get(json_object_get(response, "result"), "tools");
    size_t index;
    json_t *tool;
    json_array_foreach(tools, index, tool) {
        if (string_is(json_object_get(tool, "name"), name)) return tool;
    }
    return NULL;
}

static const char *first_text(json_t *response) {
    json_t *content = json_object_get(json_object_get(response, "result"), "content");
    json_t *block = json_array_get(content, 0);
    json_t *text = json_object_get(block, "text");
    return json_is_string(text) ? json_string_value(text) : NULL;
}

/* Every existing caller wants no skipped-tool report; the new schema-policy
 * tests that do want one call maelys_mcp_provider_proxy_spawn directly. */
static maelys_mcp_result_t spawn_proxy(
    const maelys_mcp_proxy_options_t *options,
    maelys_mcp_provider_t **out_provider,
    char **out_error) {
    return maelys_mcp_provider_proxy_spawn(options, out_provider, NULL, out_error);
}

/* Reaches the provider callback directly, below the registry. */
static maelys_mcp_result_t call_directly(
    maelys_mcp_provider_t *provider,
    const char *tool_name,
    char **out_error) {
    json_t *arguments = json_object();
    if (!arguments) return MAELYS_MCP_ERR_MEMORY;
    maelys_mcp_provider_request_t request = {
        .tool_name = tool_name, .arguments = arguments
    };
    maelys_mcp_provider_result_t result;
    maelys_mcp_provider_result_init(&result);
    maelys_mcp_result_t status = provider->call(
        provider->context, &request, &result, out_error);
    maelys_mcp_provider_result_clear(&result);
    json_decref(arguments);
    return status;
}

/*
 * mcp-runtime proxying mcp-runtime: the upstream is the real host binary
 * serving the real example provider, so the happy path exercises both sides of
 * the codebase and needs no fixture at all.
 */
static int test_dogfood(const char *host, const char *example_provider) {
    char *const upstream_argv[] = {
        (char *)host, (char *)"--provider", (char *)example_provider, NULL
    };
    maelys_mcp_proxy_options_t options = {
        .executable_path = host,
        .argv = upstream_argv,
        /* Generous on purpose: a cold exec of the upstream host and its own
         * child provider is the slowest thing in this file, and nothing here
         * is trying to measure it. */
        .connect_timeout_ms = 30000u,
        .call_timeout_ms = 30000u,
        /* Left unspecified on purpose: the fallback must be a gated class. */
        .default_effect = MAELYS_MCP_EFFECT_UNSPECIFIED,
        .tool_prefix = "up."
    };
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(spawn_proxy(
        &options, &provider, &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(provider != NULL);
    free(error);
    error = NULL;

    /* Pinned identity: a name absent from the connect-time snapshot is
     * refused inside the provider, before anything reaches the upstream. */
    ASSERT_TRUE(call_directly(provider, "up.example.absent", &error) ==
        MAELYS_MCP_ERR_NOT_FOUND);
    ASSERT_TRUE(error != NULL && strstr(error, "pinned upstream catalog") != NULL);
    free(error);
    error = NULL;
    /* Nothing went on the wire for that name. A stray request would have left
     * the following response arriving against an id nobody is waiting for,
     * which faults the transport; the upstream is instead still usable. */
    ASSERT_TRUE(call_directly(provider, "up.example.progress", &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;

    harness_t harness = {0};
    ASSERT_TRUE(harness_open(&harness, provider) == 0);

    json_t *response = dispatch(harness.channel,
        request_with(1, "tools/list", json_object()));
    ASSERT_TRUE(response != NULL);
    json_t *echo = find_tool(response, "up.example.echo");
    ASSERT_TRUE(echo != NULL);
    ASSERT_TRUE(find_tool(response, "up.example.sum") != NULL);
    ASSERT_TRUE(find_tool(response, "up.example.progress") != NULL);
    /* example.events has the execute effect the upstream host filters out of
     * its own tools/list, so the snapshot never saw it. */
    ASSERT_TRUE(find_tool(response, "up.example.events") == NULL);
    ASSERT_TRUE(json_array_size(json_object_get(
        json_object_get(response, "result"), "tools")) == 3u);
    ASSERT_TRUE(string_is(json_object_get(
        json_object_get(echo, "_meta"), "org.maelys/toolEffect"), "execute"));
    ASSERT_TRUE(string_is(json_object_get(echo, "title"), "Echo a message"));
    json_decref(response);

    response = dispatch(harness.channel, request_with(2, "tools/call",
        json_pack("{s:s,s:{s:s}}", "name", "up.example.echo",
            "arguments", "message", "proxied")));
    ASSERT_TRUE(response != NULL);
    json_t *structured = json_object_get(
        json_object_get(response, "result"), "structuredContent");
    ASSERT_TRUE(json_is_object(structured));
    ASSERT_TRUE(string_is(json_object_get(structured, "message"), "proxied"));
    json_decref(response);

    /*
     * Progress across two process boundaries. The token asserted here is the
     * one this client supplied: the proxy generates its own for the upstream
     * leg, and the relay must not leak it downstream.
     */
    json_t *params = json_pack("{s:s,s:{}}", "name", "up.example.progress", "arguments");
    json_t *meta = modern_meta();
    ASSERT_TRUE(params != NULL && meta != NULL);
    ASSERT_TRUE(json_object_set_new(meta, "progressToken", json_string("client-token")) == 0);
    ASSERT_TRUE(json_object_set_new(params, "_meta", meta) == 0);
    json_t *request = json_pack("{s:s,s:i,s:s,s:o}",
        "jsonrpc", "2.0", "id", 3, "method", "tools/call", "params", params);
    ASSERT_TRUE(maelys_mcp_channel_handle(harness.channel, request) == MAELYS_MCP_OK);
    json_decref(request);
    for (int step = 0; step <= 2; ++step) {
        json_t *frame = next_message(harness.channel);
        ASSERT_TRUE(frame != NULL);
        ASSERT_TRUE(string_is(json_object_get(frame, "method"),
            "notifications/progress"));
        json_t *frame_params = json_object_get(frame, "params");
        ASSERT_TRUE(string_is(json_object_get(frame_params, "progressToken"),
            "client-token"));
        ASSERT_TRUE(json_number_value(
            json_object_get(frame_params, "progress")) == step * 50.0);
        ASSERT_TRUE(json_number_value(json_object_get(frame_params, "total")) == 100.0);
        json_decref(frame);
    }
    response = next_message(harness.channel);
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(json_integer_value(json_object_get(response, "id")) == 3);
    json_decref(response);

    /* No token, no frames: the very next message is the response itself. */
    request = request_with(4, "tools/call",
        json_pack("{s:s,s:{}}", "name", "up.example.progress", "arguments"));
    ASSERT_TRUE(maelys_mcp_channel_handle(harness.channel, request) == MAELYS_MCP_OK);
    json_decref(request);
    response = next_message(harness.channel);
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(json_integer_value(json_object_get(response, "id")) == 4);
    json_decref(response);

    ASSERT_TRUE(harness_close(&harness) == 0);
    return 0;
}

/* An upstream that refuses server/discover falls back to the initialize
 * handshake, and everything above it works identically. */
static int test_legacy_upstream(const char *upstream) {
    maelys_mcp_proxy_options_t options = {
        .executable_path = upstream,
        .connect_timeout_ms = 30000u,
        .call_timeout_ms = 30000u,
        .default_effect = MAELYS_MCP_EFFECT_READ
    };
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(spawn_proxy(
        &options, &provider, &error) == MAELYS_MCP_OK);
    free(error);
    harness_t harness = {0};
    ASSERT_TRUE(harness_open(&harness, provider) == 0);
    json_t *response = dispatch(harness.channel,
        request_with(1, "tools/list", json_object()));
    ASSERT_TRUE(response != NULL);
    json_t *tool = find_tool(response, "legacy.echo");
    ASSERT_TRUE(tool != NULL);
    ASSERT_TRUE(string_is(json_object_get(
        json_object_get(tool, "_meta"), "org.maelys/toolEffect"), "read"));
    json_decref(response);
    response = dispatch(harness.channel, request_with(2, "tools/call",
        json_pack("{s:s,s:{}}", "name", "legacy.echo", "arguments")));
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(first_text(response) != NULL &&
        strcmp(first_text(response), "legacy-ok") == 0);
    json_decref(response);
    ASSERT_TRUE(harness_close(&harness) == 0);
    return 0;
}

/* An upstream JSON-RPC error becomes a provider error carrying the upstream's
 * own message, which the tools module surfaces as an isError result. */
static int test_erroring_upstream(const char *upstream) {
    maelys_mcp_proxy_options_t options = {
        .executable_path = upstream,
        .connect_timeout_ms = 30000u,
        .call_timeout_ms = 30000u,
        .default_effect = MAELYS_MCP_EFFECT_READ
    };
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(spawn_proxy(
        &options, &provider, &error) == MAELYS_MCP_OK);
    free(error);
    harness_t harness = {0};
    ASSERT_TRUE(harness_open(&harness, provider) == 0);
    json_t *response = dispatch(harness.channel, request_with(1, "tools/call",
        json_pack("{s:s,s:{}}", "name", "proxy.probe", "arguments")));
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(json_is_true(json_object_get(
        json_object_get(response, "result"), "isError")));
    ASSERT_TRUE(first_text(response) != NULL &&
        strcmp(first_text(response), "upstream refused the call") == 0);
    json_decref(response);
    ASSERT_TRUE(harness_close(&harness) == 0);
    return 0;
}

/*
 * Tolerance: an unknown id-less notification is dropped, and a genuine
 * server-to-client request is refused with -32601 rather than faulting the
 * transport. The upstream reports both back inside the result it then sends.
 */
static int test_chatty_upstream(const char *upstream) {
    maelys_mcp_proxy_options_t options = {
        .executable_path = upstream,
        .connect_timeout_ms = 30000u,
        .call_timeout_ms = 30000u,
        .default_effect = MAELYS_MCP_EFFECT_READ
    };
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(spawn_proxy(
        &options, &provider, &error) == MAELYS_MCP_OK);
    free(error);
    harness_t harness = {0};
    ASSERT_TRUE(harness_open(&harness, provider) == 0);
    json_t *response = dispatch(harness.channel, request_with(1, "tools/call",
        json_pack("{s:s,s:{}}", "name", "proxy.probe", "arguments")));
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(first_text(response) != NULL &&
        strcmp(first_text(response), "chatty-ok") == 0);
    json_decref(response);
    ASSERT_TRUE(harness_close(&harness) == 0);
    return 0;
}

/* An upstream that dies mid-call produces a descriptive failure, and every
 * later call fails against it immediately rather than waiting out a deadline. */
static int test_dying_upstream(const char *upstream) {
    maelys_mcp_proxy_options_t options = {
        .executable_path = upstream,
        .connect_timeout_ms = 30000u,
        /* Deliberately long: the point of the second call is that it does not
         * wait for this deadline at all. */
        .call_timeout_ms = 10000u,
        .default_effect = MAELYS_MCP_EFFECT_READ
    };
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(spawn_proxy(
        &options, &provider, &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    ASSERT_TRUE(call_directly(provider, "proxy.probe", &error) != MAELYS_MCP_OK);
    ASSERT_TRUE(error != NULL && strstr(error, "upstream MCP") != NULL);
    char *first_failure = error;
    error = NULL;
    /* Fast, and still reporting the original diagnosis rather than whatever
     * symptom writing to a dead socket produces next. */
    long long started = milliseconds();
    ASSERT_TRUE(call_directly(provider, "proxy.probe", &error) != MAELYS_MCP_OK);
    ASSERT_TRUE(milliseconds() - started < 1000LL);
    ASSERT_TRUE(error != NULL && strcmp(error, first_failure) == 0);
    free(first_failure);
    free(error);
    maelys_mcp_provider_destroy(provider);
    return 0;
}

/*
 * schema_policy (docs/mcp-proxy.md, "Schema policy"): the exotic-schema
 * upstream advertises one valid tool (proxy.good) and one whose inputSchema
 * maelys_mcp_validate_schema_definition rejects (proxy.exotic, an empty
 * oneOf). Default STRICT keeps failing the whole connect; SKIP and
 * PASSTHROUGH are the two alternatives this change adds.
 */
static int test_schema_policy_strict(const char *upstream) {
    maelys_mcp_proxy_options_t options = {
        .executable_path = upstream,
        .connect_timeout_ms = 30000u,
        .call_timeout_ms = 30000u,
        .default_effect = MAELYS_MCP_EFFECT_READ
        /* schema_policy left unset: MAELYS_MCP_PROXY_SCHEMA_STRICT is 0. */
    };
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(spawn_proxy(&options, &provider, &error) == MAELYS_MCP_ERR_PROTOCOL);
    ASSERT_TRUE(provider == NULL);
    ASSERT_TRUE(error != NULL);
    free(error);
    return 0;
}

static int test_schema_policy_skip(const char *upstream) {
    maelys_mcp_proxy_options_t options = {
        .executable_path = upstream,
        .connect_timeout_ms = 30000u,
        .call_timeout_ms = 30000u,
        .default_effect = MAELYS_MCP_EFFECT_READ,
        .schema_policy = MAELYS_MCP_PROXY_SCHEMA_SKIP
    };
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    char *skipped = NULL;
    ASSERT_TRUE(maelys_mcp_provider_proxy_spawn(
        &options, &provider, &skipped, &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    ASSERT_TRUE(skipped != NULL && strcmp(skipped, "proxy.exotic") == 0);
    free(skipped);

    harness_t harness = {0};
    ASSERT_TRUE(harness_open(&harness, provider) == 0);
    json_t *response = dispatch(harness.channel,
        request_with(1, "tools/list", json_object()));
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(find_tool(response, "proxy.good") != NULL);
    ASSERT_TRUE(find_tool(response, "proxy.exotic") == NULL);
    ASSERT_TRUE(json_array_size(json_object_get(
        json_object_get(response, "result"), "tools")) == 1u);
    json_decref(response);

    response = dispatch(harness.channel, request_with(2, "tools/call",
        json_pack("{s:s,s:{}}", "name", "proxy.good", "arguments")));
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(first_text(response) != NULL &&
        strcmp(first_text(response), "good-ok") == 0);
    json_decref(response);

    /* Absent from the snapshot: refused inside the provider, before any byte
     * reaches the upstream - the same pinned-identity path test_dogfood
     * exercises for an ordinary unknown name. */
    ASSERT_TRUE(call_directly(provider, "proxy.exotic", &error) ==
        MAELYS_MCP_ERR_NOT_FOUND);
    ASSERT_TRUE(error != NULL && strstr(error, "pinned upstream catalog") != NULL);
    free(error);

    ASSERT_TRUE(harness_close(&harness) == 0);
    return 0;
}

static int test_schema_policy_skip_nothing_skipped(
    const char *host, const char *example_provider) {
    char *const upstream_argv[] = {
        (char *)host, (char *)"--provider", (char *)example_provider, NULL
    };
    maelys_mcp_proxy_options_t options = {
        .executable_path = host,
        .argv = upstream_argv,
        .connect_timeout_ms = 30000u,
        .call_timeout_ms = 30000u,
        .default_effect = MAELYS_MCP_EFFECT_READ,
        .schema_policy = MAELYS_MCP_PROXY_SCHEMA_SKIP
    };
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    char *skipped = (char *)0x1; /* poisoned: spawn must set this to NULL */
    ASSERT_TRUE(maelys_mcp_provider_proxy_spawn(
        &options, &provider, &skipped, &error) == MAELYS_MCP_OK);
    free(error);
    ASSERT_TRUE(skipped == NULL);
    maelys_mcp_provider_destroy(provider);
    return 0;
}

static int test_schema_policy_passthrough(const char *upstream) {
    maelys_mcp_proxy_options_t options = {
        .executable_path = upstream,
        .connect_timeout_ms = 30000u,
        .call_timeout_ms = 30000u,
        .default_effect = MAELYS_MCP_EFFECT_READ,
        .schema_policy = MAELYS_MCP_PROXY_SCHEMA_PASSTHROUGH
    };
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(spawn_proxy(&options, &provider, &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;

    harness_t harness = {0};
    ASSERT_TRUE(harness_open(&harness, provider) == 0);
    json_t *response = dispatch(harness.channel,
        request_with(1, "tools/list", json_object()));
    ASSERT_TRUE(response != NULL);
    json_t *exotic = find_tool(response, "proxy.exotic");
    ASSERT_TRUE(exotic != NULL);
    /* The permissive schema this runtime's own validator accepts, not the
     * upstream's unsupported one. */
    json_t *schema = json_object_get(exotic, "inputSchema");
    ASSERT_TRUE(json_is_object(schema));
    ASSERT_TRUE(json_object_size(schema) == 1u);
    ASSERT_TRUE(string_is(json_object_get(schema, "type"), "object"));
    ASSERT_TRUE(find_tool(response, "proxy.good") != NULL);
    ASSERT_TRUE(json_array_size(json_object_get(
        json_object_get(response, "result"), "tools")) == 2u);
    json_decref(response);

    /* Callable - arguments go upstream as-is, and this fixture's upstream is
     * the one that rejects them, standing in for real upstream validation. */
    response = dispatch(harness.channel, request_with(2, "tools/call",
        json_pack("{s:s,s:{}}", "name", "proxy.exotic", "arguments")));
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(json_is_true(json_object_get(
        json_object_get(response, "result"), "isError")));
    ASSERT_TRUE(first_text(response) != NULL &&
        strcmp(first_text(response), "exotic tool arguments rejected by upstream") == 0);
    json_decref(response);

    response = dispatch(harness.channel, request_with(3, "tools/call",
        json_pack("{s:s,s:{}}", "name", "proxy.good", "arguments")));
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(first_text(response) != NULL &&
        strcmp(first_text(response), "good-ok") == 0);
    json_decref(response);

    ASSERT_TRUE(harness_close(&harness) == 0);
    return 0;
}

static int test_arguments(const char *upstream) {
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    maelys_mcp_proxy_options_t options = {
        .executable_path = "relative/path", .default_effect = MAELYS_MCP_EFFECT_READ
    };
    ASSERT_TRUE(spawn_proxy(
        &options, &provider, &error) == MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(provider == NULL && error != NULL);
    free(error);
    error = NULL;
    options.executable_path = upstream;
    options.tool_prefix = "";
    ASSERT_TRUE(spawn_proxy(
        &options, &provider, &error) == MAELYS_MCP_ERR_ARGUMENT);
    free(error);
    error = NULL;
    /* Nothing on the far end speaks MCP, so neither era negotiates. */
    options.tool_prefix = NULL;
    options.executable_path = "/usr/bin/false";
    options.connect_timeout_ms = 5000u;
    ASSERT_TRUE(spawn_proxy(
        &options, &provider, &error) != MAELYS_MCP_OK);
    ASSERT_TRUE(provider == NULL && error != NULL);
    free(error);
    return 0;
}

int main(int argc, char **argv) {
    ASSERT_TRUE(argc == 8);
    ASSERT_TRUE(test_arguments(argv[3]) == 0);
    ASSERT_TRUE(test_dogfood(argv[1], argv[2]) == 0);
    ASSERT_TRUE(test_legacy_upstream(argv[3]) == 0);
    ASSERT_TRUE(test_erroring_upstream(argv[4]) == 0);
    ASSERT_TRUE(test_chatty_upstream(argv[5]) == 0);
    ASSERT_TRUE(test_dying_upstream(argv[6]) == 0);
    ASSERT_TRUE(test_schema_policy_strict(argv[7]) == 0);
    ASSERT_TRUE(test_schema_policy_skip(argv[7]) == 0);
    ASSERT_TRUE(test_schema_policy_skip_nothing_skipped(argv[1], argv[2]) == 0);
    ASSERT_TRUE(test_schema_policy_passthrough(argv[7]) == 0);
    puts("test_mcp_proxy: OK");
    return 0;
}
