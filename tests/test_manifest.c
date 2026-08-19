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
    return expect_rejected(
        "{\"manifestVersion\":2,\"providers\":[{\"type\":\"native\",\"path\":\"/a\"}]}",
        "manifestVersion must be 1");
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

int main(void) {
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
        {"missing_path", test_missing_path}
    };
    return maelys_run_tests(cases, sizeof(cases) / sizeof(cases[0]));
}
