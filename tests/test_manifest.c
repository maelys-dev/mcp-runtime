#include "host/manifest.h"
#include "tests/test_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * manifest_load takes a file path (it is what --manifest reads), so each
 * case here writes its JSON to a private temp file, loads it, and cleans up.
 */
static int write_temp_file(const char *content, char *out_path, size_t out_path_size) {
    (void)snprintf(out_path, out_path_size, "/tmp/maelys-manifest-test-XXXXXX");
    int fd = mkstemp(out_path);
    if (fd < 0) return -1;
    FILE *file = fdopen(fd, "w");
    if (!file) {
        close(fd);
        unlink(out_path);
        return -1;
    }
    size_t length = strlen(content);
    int ok = fwrite(content, 1u, length, file) == length;
    if (fclose(file) != 0) ok = 0;
    return ok ? 0 : -1;
}

static maelys_mcp_result_t load_text(
    const char *content,
    manifest_t *out_manifest,
    char **out_error) {
    char path[64];
    ASSERT_TRUE(write_temp_file(content, path, sizeof(path)) == 0);
    maelys_mcp_result_t status = manifest_load(path, out_manifest, out_error);
    unlink(path);
    return status;
}

static int expect_rejected(const char *content, const char *must_contain) {
    manifest_t manifest;
    char *error = NULL;
    maelys_mcp_result_t status = load_text(content, &manifest, &error);
    ASSERT_TRUE(status != MAELYS_MCP_OK);
    ASSERT_TRUE(error != NULL);
    if (!strstr(error, must_contain)) {
        fprintf(stderr, "expected error to contain \"%s\", got \"%s\"\n", must_contain, error);
        free(error);
        return 1;
    }
    free(error);
    /* A rejected manifest must never leave a partially built result behind. */
    ASSERT_TRUE(manifest.provider_count == 0 && manifest.providers == NULL);
    manifest_clear(&manifest);
    return 0;
}

static int test_missing_file(void) {
    manifest_t manifest;
    char *error = NULL;
    ASSERT_TRUE(manifest_load(
        "/tmp/maelys-manifest-test-does-not-exist", &manifest, &error) != MAELYS_MCP_OK);
    ASSERT_TRUE(error != NULL);
    free(error);
    return 0;
}

static int test_json_syntax_error(void) {
    return expect_rejected("{not json", "syntax error");
}

static int test_valid_manifest(void) {
    const char *text =
        "{"
        "\"manifestVersion\":1,"
        "\"providers\":["
            "{\"type\":\"native\",\"path\":\"/abs/example-provider\","
             "\"describeTimeoutMs\":5000,\"callTimeoutMs\":300000,"
             "\"shutdownTimeoutMs\":2000,\"maxMessageBytes\":1048576},"
            "{\"type\":\"mcp-proxy\",\"path\":\"/abs/github-mcp\",\"argv\":[\"--stdio\"],"
             "\"toolPrefix\":\"gh.\",\"defaultEffect\":\"execute\",\"schemaPolicy\":\"skip\","
             "\"connectTimeoutMs\":10000,\"callTimeoutMs\":30000,\"maxMessageBytes\":1048576}"
        "],"
        "\"allowEffects\":[\"apply\",\"commit\"]"
        "}";
    manifest_t manifest;
    char *error = NULL;
    ASSERT_TRUE(load_text(text, &manifest, &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(error == NULL);
    ASSERT_TRUE(manifest.version == 1);
    ASSERT_TRUE(manifest.provider_count == 2u);
    ASSERT_TRUE(manifest.allowed_effects ==
        ((1u << (unsigned int)MAELYS_MCP_EFFECT_APPLY) |
         (1u << (unsigned int)MAELYS_MCP_EFFECT_COMMIT)));

    const manifest_provider_t *native = &manifest.providers[0];
    ASSERT_TRUE(native->type == MANIFEST_PROVIDER_NATIVE);
    ASSERT_TRUE(strcmp(native->path, "/abs/example-provider") == 0);
    ASSERT_TRUE(native->describe_timeout_ms == 5000u);
    ASSERT_TRUE(native->call_timeout_ms == 300000u);
    ASSERT_TRUE(native->shutdown_timeout_ms == 2000u);
    ASSERT_TRUE(native->max_message_bytes == 1048576u);
    ASSERT_TRUE(native->argv == NULL && native->argv_count == 0u);

    const manifest_provider_t *proxy = &manifest.providers[1];
    ASSERT_TRUE(proxy->type == MANIFEST_PROVIDER_MCP_PROXY);
    ASSERT_TRUE(strcmp(proxy->path, "/abs/github-mcp") == 0);
    ASSERT_TRUE(proxy->argv != NULL && proxy->argv_count == 1u);
    ASSERT_TRUE(strcmp(proxy->argv[0], "--stdio") == 0);
    ASSERT_TRUE(proxy->argv[1] == NULL);
    ASSERT_TRUE(strcmp(proxy->tool_prefix, "gh.") == 0);
    ASSERT_TRUE(proxy->default_effect == MAELYS_MCP_EFFECT_EXECUTE);
    ASSERT_TRUE(proxy->schema_policy == MAELYS_MCP_PROXY_SCHEMA_SKIP);
    ASSERT_TRUE(proxy->connect_timeout_ms == 10000u);
    ASSERT_TRUE(proxy->call_timeout_ms == 30000u);
    ASSERT_TRUE(proxy->max_message_bytes == 1048576u);

    manifest_clear(&manifest);
    return 0;
}

static int test_valid_manifest_minimal_defaults(void) {
    const char *text =
        "{\"manifestVersion\":1,\"providers\":["
        "{\"type\":\"native\",\"path\":\"/abs/p\"},"
        "{\"type\":\"mcp-proxy\",\"path\":\"/abs/q\"}"
        "]}";
    manifest_t manifest;
    char *error = NULL;
    ASSERT_TRUE(load_text(text, &manifest, &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(manifest.provider_count == 2u);
    ASSERT_TRUE(manifest.allowed_effects == 0u);
    const manifest_provider_t *native = &manifest.providers[0];
    ASSERT_TRUE(native->describe_timeout_ms == 0u && native->call_timeout_ms == 0u &&
        native->shutdown_timeout_ms == 0u && native->max_message_bytes == 0u);
    const manifest_provider_t *proxy = &manifest.providers[1];
    ASSERT_TRUE(proxy->argv == NULL && proxy->argv_count == 0u);
    ASSERT_TRUE(proxy->tool_prefix == NULL);
    ASSERT_TRUE(proxy->default_effect == MAELYS_MCP_EFFECT_UNSPECIFIED);
    ASSERT_TRUE(proxy->schema_policy == MAELYS_MCP_PROXY_SCHEMA_STRICT);
    ASSERT_TRUE(proxy->connect_timeout_ms == 0u && proxy->call_timeout_ms == 0u &&
        proxy->max_message_bytes == 0u);
    manifest_clear(&manifest);
    return 0;
}

static int test_unknown_key_top_level(void) {
    return expect_rejected(
        "{\"manifestVersion\":1,\"providers\":[{\"type\":\"native\",\"path\":\"/a\"}],"
        "\"bogus\":true}",
        "unknown key \"bogus\" in manifest");
}

static int test_unknown_key_provider_level(void) {
    return expect_rejected(
        "{\"manifestVersion\":1,\"providers\":["
        "{\"type\":\"native\",\"path\":\"/a\",\"bogus\":true}]}",
        "unknown key \"bogus\" in providers[0]");
}

static int test_unknown_key_wrong_type_for_provider(void) {
    /* argv is a real key, just not a native one - proves per-type keys. */
    return expect_rejected(
        "{\"manifestVersion\":1,\"providers\":["
        "{\"type\":\"native\",\"path\":\"/a\",\"argv\":[\"x\"]}]}",
        "unknown key \"argv\" in providers[0]");
}

static int test_missing_manifest_version(void) {
    return expect_rejected(
        "{\"providers\":[{\"type\":\"native\",\"path\":\"/a\"}]}",
        "manifestVersion");
}

static int test_wrong_manifest_version(void) {
    /* manifestVersion 2 is a valid version (test_v2_*, below) - this proves
     * only that versions outside {1, 2} are still rejected. */
    return expect_rejected(
        "{\"manifestVersion\":3,\"providers\":[{\"type\":\"native\",\"path\":\"/a\"}]}",
        "manifestVersion must be 1 or 2");
}

static int test_empty_providers(void) {
    return expect_rejected(
        "{\"manifestVersion\":1,\"providers\":[]}",
        "providers must be a non-empty array");
}

static int test_relative_path(void) {
    return expect_rejected(
        "{\"manifestVersion\":1,\"providers\":[{\"type\":\"native\",\"path\":\"rel/path\"}]}",
        "providers[0].path must be an absolute path");
}

static int test_bad_schema_policy(void) {
    return expect_rejected(
        "{\"manifestVersion\":1,\"providers\":["
        "{\"type\":\"mcp-proxy\",\"path\":\"/a\",\"schemaPolicy\":\"bogus\"}]}",
        "providers[0].schemaPolicy must be one of");
}

static int test_bad_default_effect(void) {
    return expect_rejected(
        "{\"manifestVersion\":1,\"providers\":["
        "{\"type\":\"mcp-proxy\",\"path\":\"/a\",\"defaultEffect\":\"bogus\"}]}",
        "providers[0].defaultEffect is not a known effect class");
}

static int test_bad_allow_effect(void) {
    return expect_rejected(
        "{\"manifestVersion\":1,\"providers\":[{\"type\":\"native\",\"path\":\"/a\"}],"
        "\"allowEffects\":[\"read\"]}",
        "allowEffects[0] must be one of");
}

static int test_argv_not_array_of_strings(void) {
    return expect_rejected(
        "{\"manifestVersion\":1,\"providers\":["
        "{\"type\":\"mcp-proxy\",\"path\":\"/a\",\"argv\":[\"ok\",7]}]}",
        "providers[0].argv[1] must be a string");
}

static int test_bad_provider_type(void) {
    return expect_rejected(
        "{\"manifestVersion\":1,\"providers\":["
        "{\"type\":\"bogus\",\"path\":\"/a\"}]}",
        "providers[0].type must be \"native\" or \"mcp-proxy\"");
}

static int test_provider_not_object(void) {
    return expect_rejected(
        "{\"manifestVersion\":1,\"providers\":[\"not-an-object\"]}",
        "providers[0] must be an object");
}

static int test_missing_path(void) {
    return expect_rejected(
        "{\"manifestVersion\":1,\"providers\":[{\"type\":\"native\"}]}",
        "providers[0] is missing required key \"path\"");
}

/*
 * ---- manifest v2: "args" and "executionProfile" (docs/manifest.md,
 * docs/launch-contract-design.md) ----
 */

static int test_v2_native_args_and_profile(void) {
    const char *text =
        "{\"manifestVersion\":2,\"providers\":["
        "{\"type\":\"native\",\"path\":\"/abs/example-provider\","
        "\"args\":[\"--root\",\"/srv/docs\"],\"executionProfile\":\"trusted-local\"}"
        "]}";
    manifest_t manifest;
    char *error = NULL;
    ASSERT_TRUE(load_text(text, &manifest, &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(error == NULL);
    ASSERT_TRUE(manifest.version == 2);
    const manifest_provider_t *native = &manifest.providers[0];
    ASSERT_TRUE(native->args != NULL && native->args_count == 2u);
    ASSERT_TRUE(strcmp(native->args[0], "--root") == 0);
    ASSERT_TRUE(strcmp(native->args[1], "/srv/docs") == 0);
    ASSERT_TRUE(native->args[2] == NULL);
    ASSERT_TRUE(native->execution_profile != NULL);
    ASSERT_TRUE(strcmp(native->execution_profile, "trusted-local") == 0);
    manifest_clear(&manifest);
    return 0;
}

static int test_v2_proxy_execution_profile(void) {
    const char *text =
        "{\"manifestVersion\":2,\"providers\":["
        "{\"type\":\"mcp-proxy\",\"path\":\"/abs/github-mcp\",\"argv\":[\"--stdio\"],"
        "\"executionProfile\":\"trusted-local\"}"
        "]}";
    manifest_t manifest;
    char *error = NULL;
    ASSERT_TRUE(load_text(text, &manifest, &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(manifest.version == 2);
    const manifest_provider_t *proxy = &manifest.providers[0];
    ASSERT_TRUE(proxy->execution_profile != NULL);
    ASSERT_TRUE(strcmp(proxy->execution_profile, "trusted-local") == 0);
    /* v2 changes nothing about how argv is carried - the proxy asymmetry
     * documented in docs/launch-contract-design.md holds unchanged. */
    ASSERT_TRUE(proxy->argv != NULL && proxy->argv_count == 1u);
    ASSERT_TRUE(strcmp(proxy->argv[0], "--stdio") == 0);
    manifest_clear(&manifest);
    return 0;
}

/*
 * Absence and "trusted-local" are DISTINCT values (the design's accepted
 * decision 2): a v2 manifest that sets neither key must produce NULL, not
 * some default string - a v2 document with nothing new to say is not
 * distinguishable from v1 at the C struct level.
 */
static int test_v2_defaults_absent(void) {
    const char *text =
        "{\"manifestVersion\":2,\"providers\":["
        "{\"type\":\"native\",\"path\":\"/abs/p\"},"
        "{\"type\":\"mcp-proxy\",\"path\":\"/abs/q\"}"
        "]}";
    manifest_t manifest;
    char *error = NULL;
    ASSERT_TRUE(load_text(text, &manifest, &error) == MAELYS_MCP_OK);
    const manifest_provider_t *native = &manifest.providers[0];
    ASSERT_TRUE(native->args == NULL && native->args_count == 0u);
    ASSERT_TRUE(native->execution_profile == NULL);
    const manifest_provider_t *proxy = &manifest.providers[1];
    ASSERT_TRUE(proxy->execution_profile == NULL);
    manifest_clear(&manifest);
    return 0;
}

static int test_v1_rejects_args(void) {
    return expect_rejected(
        "{\"manifestVersion\":1,\"providers\":["
        "{\"type\":\"native\",\"path\":\"/a\",\"args\":[\"--x\"]}]}",
        "unknown key \"args\" in providers[0]");
}

static int test_v1_rejects_execution_profile_native(void) {
    return expect_rejected(
        "{\"manifestVersion\":1,\"providers\":["
        "{\"type\":\"native\",\"path\":\"/a\",\"executionProfile\":\"trusted-local\"}]}",
        "unknown key \"executionProfile\" in providers[0]");
}

static int test_v1_rejects_execution_profile_proxy(void) {
    return expect_rejected(
        "{\"manifestVersion\":1,\"providers\":["
        "{\"type\":\"mcp-proxy\",\"path\":\"/a\",\"executionProfile\":\"trusted-local\"}]}",
        "unknown key \"executionProfile\" in providers[0]");
}

/* "argv" stays a native-side unknown key under v2 too: v2 does not touch the
 * native/proxy disjointness docs/manifest.md already describes, it only adds
 * one key to each side (and one to both). */
static int test_v2_argv_rejected_on_native(void) {
    return expect_rejected(
        "{\"manifestVersion\":2,\"providers\":["
        "{\"type\":\"native\",\"path\":\"/a\",\"argv\":[\"x\"]}]}",
        "unknown key \"argv\" in providers[0]");
}

static int test_v2_args_rejected_on_proxy(void) {
    return expect_rejected(
        "{\"manifestVersion\":2,\"providers\":["
        "{\"type\":\"mcp-proxy\",\"path\":\"/a\",\"args\":[\"x\"]}]}",
        "unknown key \"args\" in providers[0]");
}

static int test_v2_args_not_array(void) {
    return expect_rejected(
        "{\"manifestVersion\":2,\"providers\":["
        "{\"type\":\"native\",\"path\":\"/a\",\"args\":\"nope\"}]}",
        "providers[0].args must be an array of strings");
}

static int test_v2_args_non_string_entry(void) {
    return expect_rejected(
        "{\"manifestVersion\":2,\"providers\":["
        "{\"type\":\"native\",\"path\":\"/a\",\"args\":[\"ok\",7]}]}",
        "providers[0].args[1] must be a string");
}

static int test_v2_execution_profile_wrong_type(void) {
    return expect_rejected(
        "{\"manifestVersion\":2,\"providers\":["
        "{\"type\":\"native\",\"path\":\"/a\",\"executionProfile\":7}]}",
        "providers[0].executionProfile must be a non-empty string");
}

static int test_v2_execution_profile_empty_string(void) {
    return expect_rejected(
        "{\"manifestVersion\":2,\"providers\":["
        "{\"type\":\"mcp-proxy\",\"path\":\"/a\",\"executionProfile\":\"\"}]}",
        "providers[0].executionProfile must be a non-empty string");
}

/* Builds a v2 native manifest whose "args" array holds `count` short string
 * entries - caller-owned (free() it), or NULL on allocation failure. */
static char *build_manifest_with_args_count(size_t count) {
    size_t capacity = 128u + count * 6u;
    char *text = malloc(capacity);
    if (!text) return NULL;
    int written = snprintf(text, capacity,
        "{\"manifestVersion\":2,\"providers\":["
        "{\"type\":\"native\",\"path\":\"/a\",\"args\":[");
    if (written < 0) { free(text); return NULL; }
    size_t used = (size_t)written;
    for (size_t index = 0; index < count; ++index) {
        written = snprintf(text + used, capacity - used, "%s\"x\"", index ? "," : "");
        if (written < 0) { free(text); return NULL; }
        used += (size_t)written;
    }
    written = snprintf(text + used, capacity - used, "]}]}");
    if (written < 0) { free(text); return NULL; }
    return text;
}

/* 64 is the accepted bound: proves the boundary is inclusive, not just that
 * something past it fails. */
static int test_v2_args_at_entry_bound(void) {
    char *text = build_manifest_with_args_count(64u);
    ASSERT_TRUE(text != NULL);
    manifest_t manifest;
    char *error = NULL;
    maelys_mcp_result_t status = load_text(text, &manifest, &error);
    free(text);
    ASSERT_TRUE(status == MAELYS_MCP_OK);
    ASSERT_TRUE(manifest.providers[0].args_count == 64u);
    manifest_clear(&manifest);
    return 0;
}

static int test_v2_args_too_many_entries(void) {
    char *text = build_manifest_with_args_count(65u);
    ASSERT_TRUE(text != NULL);
    int result = expect_rejected(text,
        "providers[0].args must have at most 64 entries, found 65");
    free(text);
    return result;
}

/* Builds a v2 native manifest with one "args" entry made of `length` 'x'
 * characters - caller-owned (free() it), or NULL on allocation failure. */
static char *build_manifest_with_arg_length(size_t length) {
    size_t capacity = length + 128u;
    char *text = malloc(capacity);
    char *value = text ? malloc(length + 1u) : NULL;
    if (!text || !value) {
        free(text);
        free(value);
        return NULL;
    }
    memset(value, 'x', length);
    value[length] = '\0';
    (void)snprintf(text, capacity,
        "{\"manifestVersion\":2,\"providers\":["
        "{\"type\":\"native\",\"path\":\"/a\",\"args\":[\"%s\"]}]}", value);
    free(value);
    return text;
}

/* The bound counts one separator byte per entry (a trailing NUL, the way real
 * argv memory needs one between entries), so a single 8191-byte entry sits
 * exactly at the 8192-byte bound and must be accepted. */
static int test_v2_args_at_byte_bound(void) {
    char *text = build_manifest_with_arg_length(8191u);
    ASSERT_TRUE(text != NULL);
    manifest_t manifest;
    char *error = NULL;
    maelys_mcp_result_t status = load_text(text, &manifest, &error);
    free(text);
    ASSERT_TRUE(status == MAELYS_MCP_OK);
    ASSERT_TRUE(manifest.providers[0].args_count == 1u);
    ASSERT_TRUE(strlen(manifest.providers[0].args[0]) == 8191u);
    manifest_clear(&manifest);
    return 0;
}

static int test_v2_args_too_many_bytes(void) {
    char *text = build_manifest_with_arg_length(8192u);
    ASSERT_TRUE(text != NULL);
    int result = expect_rejected(text,
        "providers[0].args must be at most 8192 bytes total (including separators), found 8193");
    free(text);
    return result;
}

/*
 * The end-to-end proof the design calls for: manifest parsing alone cannot
 * show that "args" reaches a real child's argv, only a real spawn can.
 * argv-echo-provider (tests/helpers/adversarial_provider.c) reports
 * argv[1..argc-1] back in a tool description; a real maelys-mcp process would
 * compile that vector as {"--provider", "--root", "/srv/docs", "--verbose"}
 * (docs/launch-contract-design.md, "Two layers of argv"), so finding exactly
 * that string proves the wiring from manifest_load through
 * maelys_mcp_provider_spawn_with_args to a real execve, not just that
 * spawn_process was called.
 */
static const char *g_argv_echo_provider;

static json_t *manifest_test_dispatch(maelys_mcp_channel_t *channel, json_t *request) {
    maelys_mcp_result_t status = maelys_mcp_channel_handle(channel, request);
    json_decref(request);
    if (status != MAELYS_MCP_OK) return NULL;
    json_t *response = NULL;
    return maelys_mcp_channel_next(channel, 5000u, &response) == MAELYS_MCP_OK ?
        response : NULL;
}

static int test_v2_args_reach_child_argv(void) {
    ASSERT_TRUE(g_argv_echo_provider != NULL);
    char text[512];
    (void)snprintf(text, sizeof(text),
        "{\"manifestVersion\":2,\"providers\":["
        "{\"type\":\"native\",\"path\":\"%s\","
        "\"args\":[\"--root\",\"/srv/docs\",\"--verbose\"]}"
        "]}", g_argv_echo_provider);
    manifest_t manifest;
    char *error = NULL;
    ASSERT_TRUE(load_text(text, &manifest, &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(error == NULL);
    const manifest_provider_t *entry = &manifest.providers[0];
    ASSERT_TRUE(entry->args_count == 3u);

    maelys_mcp_provider_process_options_t options = {
        .executable_path = entry->path,
        .max_message_bytes = 65536u,
        .describe_timeout_ms = 30000u
    };
    maelys_mcp_provider_t *provider = NULL;
    char *spawn_error = NULL;
    maelys_mcp_result_t status = maelys_mcp_provider_spawn_with_args(
        &options, entry->args, entry->execution_profile, &provider, &spawn_error);
    manifest_clear(&manifest);
    if (status != MAELYS_MCP_OK) {
        fprintf(stderr, "argv-echo spawn failed: %s\n",
            spawn_error ? spawn_error : "(no message)");
        free(spawn_error);
        return 1;
    }
    free(spawn_error);

    maelys_mcp_runtime_config_t runtime_config = {
        .server_name = "test-manifest", .server_version = "1.0"
    };
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(maelys_mcp_runtime_create(&runtime_config, &runtime) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_TOOLS) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, provider, NULL) == MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

    json_t *params = json_pack("{s:{s:s,s:{s:s,s:s},s:{}}}",
        "_meta",
        "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
        "io.modelcontextprotocol/clientInfo", "name", "test", "version", "1.0",
        "io.modelcontextprotocol/clientCapabilities");
    json_t *request = json_pack("{s:s,s:I,s:s,s:o}",
        "jsonrpc", "2.0", "id", (json_int_t)1, "method", "tools/list", "params", params);
    json_t *response = manifest_test_dispatch(channel, request);
    ASSERT_TRUE(response != NULL);
    json_t *result = json_object_get(response, "result");
    ASSERT_TRUE(result != NULL && !json_object_get(response, "error"));
    json_t *tools = json_object_get(result, "tools");
    ASSERT_TRUE(json_array_size(tools) == 1u);
    json_t *tool = json_array_get(tools, 0);
    const char *description = json_string_value(json_object_get(tool, "description"));
    ASSERT_TRUE(description != NULL);
    ASSERT_TRUE(strcmp(description, "--provider,--root,/srv/docs,--verbose") == 0);
    json_decref(response);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

int main(int argc, char **argv) {
    g_argv_echo_provider = argc > 1 ? argv[1] : NULL;
    static const maelys_test_case_t cases[] = {
        {"missing_file", test_missing_file},
        {"json_syntax_error", test_json_syntax_error},
        {"valid_manifest", test_valid_manifest},
        {"valid_manifest_minimal_defaults", test_valid_manifest_minimal_defaults},
        {"unknown_key_top_level", test_unknown_key_top_level},
        {"unknown_key_provider_level", test_unknown_key_provider_level},
        {"unknown_key_wrong_type_for_provider", test_unknown_key_wrong_type_for_provider},
        {"missing_manifest_version", test_missing_manifest_version},
        {"wrong_manifest_version", test_wrong_manifest_version},
        {"empty_providers", test_empty_providers},
        {"relative_path", test_relative_path},
        {"bad_schema_policy", test_bad_schema_policy},
        {"bad_default_effect", test_bad_default_effect},
        {"bad_allow_effect", test_bad_allow_effect},
        {"argv_not_array_of_strings", test_argv_not_array_of_strings},
        {"bad_provider_type", test_bad_provider_type},
        {"provider_not_object", test_provider_not_object},
        {"missing_path", test_missing_path},
        {"v2_native_args_and_profile", test_v2_native_args_and_profile},
        {"v2_proxy_execution_profile", test_v2_proxy_execution_profile},
        {"v2_defaults_absent", test_v2_defaults_absent},
        {"v1_rejects_args", test_v1_rejects_args},
        {"v1_rejects_execution_profile_native", test_v1_rejects_execution_profile_native},
        {"v1_rejects_execution_profile_proxy", test_v1_rejects_execution_profile_proxy},
        {"v2_argv_rejected_on_native", test_v2_argv_rejected_on_native},
        {"v2_args_rejected_on_proxy", test_v2_args_rejected_on_proxy},
        {"v2_args_not_array", test_v2_args_not_array},
        {"v2_args_non_string_entry", test_v2_args_non_string_entry},
        {"v2_execution_profile_wrong_type", test_v2_execution_profile_wrong_type},
        {"v2_execution_profile_empty_string", test_v2_execution_profile_empty_string},
        {"v2_args_at_entry_bound", test_v2_args_at_entry_bound},
        {"v2_args_too_many_entries", test_v2_args_too_many_entries},
        {"v2_args_at_byte_bound", test_v2_args_at_byte_bound},
        {"v2_args_too_many_bytes", test_v2_args_too_many_bytes},
        {"v2_args_reach_child_argv", test_v2_args_reach_child_argv}
    };
    return maelys_run_tests(cases, sizeof(cases) / sizeof(cases[0]));
}
