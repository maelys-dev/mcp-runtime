#include "src/internal/internal.h"
#include "tests/test_support.h"

#include <stdlib.h>
#include <string.h>

static maelys_mcp_result_t unused_call(
    void *context,
    const char *tool_name,
    json_t *arguments,
    json_t **out_result,
    char **out_error) {
    (void)context;
    (void)tool_name;
    (void)arguments;
    (void)out_error;
    *out_result = json_object();
    return *out_result ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static json_t *parse(const char *text) {
    json_error_t error;
    return json_loads(text, JSON_REJECT_DUPLICATES, &error);
}

static int definition_status(const char *text, int require_object) {
    json_t *schema = parse(text);
    ASSERT_TRUE(schema != NULL);
    char *error = NULL;
    maelys_mcp_result_t status = maelys_mcp_validate_schema_definition(
        schema, require_object, &error);
    if (status != MAELYS_MCP_OK) ASSERT_TRUE(error != NULL);
    free(error);
    json_decref(schema);
    return status;
}

static int test_valid_nested_definition(void) {
    const char *text = "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
        "\"title\":\"input\",\"description\":\"supported subset\",\"type\":\"object\","
        "\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":20},"
        "\"count\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10},"
        "\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "\"mode\":{\"type\":\"string\",\"enum\":[\"read\",\"write\"]}},"
        "\"required\":[\"name\"],\"additionalProperties\":false}";
    ASSERT_TRUE(definition_status(text, 1) == MAELYS_MCP_OK);
    return 0;
}

static int test_unsupported_and_missing_types_rejected(void) {
    ASSERT_TRUE(definition_status("{\"type\":\"object\",\"patternProperties\":{}}", 1) == MAELYS_MCP_ERR_PROTOCOL);
    ASSERT_TRUE(definition_status("{\"properties\":{}}", 1) == MAELYS_MCP_ERR_PROTOCOL);
    ASSERT_TRUE(definition_status("{\"type\":\"unknown\"}", 0) == MAELYS_MCP_ERR_PROTOCOL);
    ASSERT_TRUE(definition_status("{\"type\":\"string\"}", 1) == MAELYS_MCP_ERR_PROTOCOL);
    return 0;
}

static int test_keyword_shapes_rejected(void) {
    static const char *invalid[] = {
        "{\"type\":\"string\",\"properties\":{}}",
        "{\"type\":\"object\",\"properties\":[]}",
        "{\"type\":\"object\",\"properties\":{},\"required\":\"x\"}",
        "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":0}",
        "{\"type\":\"array\",\"items\":true}",
        "{\"type\":\"string\",\"items\":{\"type\":\"string\"}}",
        "{\"type\":\"integer\",\"minLength\":1}",
        "{\"type\":\"string\",\"minimum\":1}"
    };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        ASSERT_TRUE(definition_status(invalid[index], 0) == MAELYS_MCP_ERR_PROTOCOL);
    }
    return 0;
}

static int test_inconsistent_bounds_and_enum_rejected(void) {
    static const char *invalid[] = {
        "{\"type\":\"string\",\"minLength\":-1}",
        "{\"type\":\"string\",\"minLength\":4,\"maxLength\":3}",
        "{\"type\":\"number\",\"minimum\":2,\"maximum\":1}",
        "{\"type\":\"string\",\"enum\":[]}",
        "{\"type\":\"string\",\"enum\":[1]}",
        "{\"type\":\"object\",\"properties\":{},\"required\":[\"missing\"]}",
        "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"string\"}},\"required\":[\"x\",\"x\"]}"
    };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        ASSERT_TRUE(definition_status(invalid[index], 0) == MAELYS_MCP_ERR_PROTOCOL);
    }
    return 0;
}

static int test_runtime_validation_matches_subset(void) {
    json_t *schema = parse("{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\",\"minLength\":2,\"maxLength\":4},"
        "\"score\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1},"
        "\"mode\":{\"type\":\"string\",\"enum\":[\"a\",\"b\"]}},"
        "\"required\":[\"name\"],\"additionalProperties\":false}");
    ASSERT_TRUE(schema != NULL);
    ASSERT_TRUE(maelys_mcp_validate_schema_definition(schema, 1, NULL) == MAELYS_MCP_OK);
    json_t *valid = parse("{\"name\":\"abc\",\"score\":0.5,\"mode\":\"a\"}");
    json_t *short_name = parse("{\"name\":\"x\"}");
    json_t *extra = parse("{\"name\":\"abc\",\"extra\":true}");
    json_t *bad_enum = parse("{\"name\":\"abc\",\"mode\":\"c\"}");
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_validate_schema(schema, valid, &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(error == NULL);
    ASSERT_TRUE(maelys_mcp_validate_schema(schema, short_name, &error) == MAELYS_MCP_ERR_ARGUMENT);
    free(error); error = NULL;
    ASSERT_TRUE(maelys_mcp_validate_schema(schema, extra, &error) == MAELYS_MCP_ERR_ARGUMENT);
    free(error); error = NULL;
    ASSERT_TRUE(maelys_mcp_validate_schema(schema, bad_enum, &error) == MAELYS_MCP_ERR_ARGUMENT);
    free(error);
    json_decref(valid);
    json_decref(short_name);
    json_decref(extra);
    json_decref(bad_enum);
    json_decref(schema);
    return 0;
}

static int test_provider_preserves_exact_schema(void) {
    json_t *input = parse("{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"integer\"}},\"required\":[\"x\"],\"additionalProperties\":false}");
    json_t *output = parse("{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"}},\"required\":[\"ok\"],\"additionalProperties\":false}");
    ASSERT_TRUE(input && output);
    maelys_mcp_tool_t tool = {
        .name = "test.schema",
        .description = "Schema parity test.",
        .input_schema = input,
        .output_schema = output,
        .effect = MAELYS_MCP_EFFECT_READ
    };
    maelys_mcp_provider_config_t config = {
        .name = "schema", .version = "1", .tools = &tool, .tool_count = 1,
        .call = unused_call
    };
    maelys_mcp_provider_t *provider = NULL;
    ASSERT_TRUE(maelys_mcp_provider_create(&config, &provider) == MAELYS_MCP_OK);
    ASSERT_TRUE(json_equal(provider->tools[0].input_schema, input));
    ASSERT_TRUE(json_equal(provider->tools[0].output_schema, output));
    maelys_mcp_provider_destroy(provider);

    json_object_set_new(input, "patternProperties", json_object());
    provider = NULL;
    ASSERT_TRUE(maelys_mcp_provider_create(&config, &provider) == MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(provider == NULL);

    json_object_del(input, "patternProperties");
    maelys_mcp_tool_t duplicate_tools[] = {tool, tool};
    config.tools = duplicate_tools;
    config.tool_count = 2;
    ASSERT_TRUE(maelys_mcp_provider_create(&config, &provider) == MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(provider == NULL);
    json_decref(input);
    json_decref(output);
    return 0;
}

int main(void) {
    static const maelys_test_case_t tests[] = {
        {"valid nested schema definition", test_valid_nested_definition},
        {"unsupported keywords and missing types rejected", test_unsupported_and_missing_types_rejected},
        {"schema keyword shapes rejected", test_keyword_shapes_rejected},
        {"inconsistent bounds, enum, required rejected", test_inconsistent_bounds_and_enum_rejected},
        {"runtime validation matches supported subset", test_runtime_validation_matches_subset},
        {"provider preserves exact schema and rejects duplicate tools", test_provider_preserves_exact_schema}
    };
    return maelys_run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
