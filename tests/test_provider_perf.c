/*
 * tools/call round-trip cost, and what the process boundary costs.
 *
 * Drives the same dispatch path twice - once against an out-of-process
 * provider, once against an in-process one doing the least work a tool can
 * do - so the difference isolates IPC from the runtime's own dispatch. The
 * figures are printed on every run, because the useful signal is the trend
 * and the split, not a pass/fail bit:
 *
 *   - the out-of-process figure is the hot path a real deployment pays;
 *   - the in-process figure is what the runtime itself costs;
 *   - the difference is the price of provider isolation, which is a feature.
 *
 * Both calls are checked for a well-formed `result`, not just timed: without
 * that, a regression turning every call into an error would still "pass" and
 * would merely be measuring the error path.
 *
 * As with test_channel_perf.c, the asserted ceiling is deliberately loose -
 * a tight timing gate in CI is a flakiness source, and the printed numbers
 * carry the real signal. This test is not part of `make tsan`: its only
 * concurrency is the provider reader thread, which test_process_provider
 * already covers there, so instrumenting it would cost time for no new
 * coverage.
 */
#include "maelys/mcp.h"
#include "tests/test_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WARMUP 200
#define ITERATIONS 2000
/* See the header comment: loose on purpose. */
#define CEILING_US 5000.0

static const char *REQUEST_TEMPLATE =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
    "\"name\":\"%s\",\"arguments\":{\"message\":\"bench\"},"
    "\"_meta\":{"
    "\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\","
    "\"io.modelcontextprotocol/clientInfo\":{\"name\":\"perf\",\"version\":\"1\"},"
    "\"io.modelcontextprotocol/clientCapabilities\":{}}}}";

static const char *provider_path;

static maelys_mcp_result_t inproc_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    (void)context; (void)request; (void)out_error;
    out_result->type = MAELYS_MCP_PROVIDER_RESULT_COMPLETE;
    out_result->structured_content = json_pack("{s:s}", "message", "bench");
    return out_result->structured_content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

/* Microseconds per tools/call, or a negative value on failure. */
static double drive(maelys_mcp_channel_t *channel, const char *tool_name) {
    char request_text[1024];
    (void)snprintf(request_text, sizeof(request_text), REQUEST_TEMPLATE, tool_name);
    struct timespec start, end;
    memset(&start, 0, sizeof(start));
    for (int index = 0; index < WARMUP + ITERATIONS; ++index) {
        if (index == WARMUP && clock_gettime(CLOCK_MONOTONIC, &start) != 0) return -1.0;
        json_t *request = json_loads(request_text, 0, NULL);
        if (!request) return -2.0;
        maelys_mcp_result_t status = maelys_mcp_channel_handle(channel, request);
        json_decref(request);
        if (status != MAELYS_MCP_OK) return -3.0;
        json_t *response = NULL;
        if (maelys_mcp_channel_next(channel, 5000u, &response) != MAELYS_MCP_OK) return -4.0;
        /* Time the success path, not an error path that silently replaced it. */
        int well_formed = json_is_object(json_object_get(response, "result")) &&
            !json_object_get(response, "error");
        json_decref(response);
        if (!well_formed) return -5.0;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) return -6.0;
    double microseconds = ((double)(end.tv_sec - start.tv_sec) * 1e9 +
        (double)(end.tv_nsec - start.tv_nsec)) / 1000.0;
    return microseconds / ITERATIONS;
}

static double measure_out_of_process(void) {
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    if (maelys_mcp_provider_spawn(provider_path, 1024u * 1024u,
            &provider, &error) != MAELYS_MCP_OK) {
        fprintf(stderr, "   spawn failed: %s\n", error ? error : "(no detail)");
        free(error);
        return -1.0;
    }
    maelys_mcp_runtime_config_t config = {
        .server_name = "provider-perf", .server_version = "1"
    };
    maelys_mcp_runtime_t *runtime = NULL;
    if (maelys_mcp_runtime_create(&config, &runtime) != MAELYS_MCP_OK) return -2.0;
    if (maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_TOOLS) != MAELYS_MCP_OK) return -3.0;
    /* The example provider also declares resources and templates, and
     * add_provider rejects a provider whose surface has no enabled module. */
    if (maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_RESOURCES) != MAELYS_MCP_OK) return -4.0;
    if (maelys_mcp_runtime_add_provider(runtime, provider, NULL) != MAELYS_MCP_OK) return -5.0;
    maelys_mcp_channel_t *channel = NULL;
    if (maelys_mcp_channel_create(runtime, NULL, &channel) != MAELYS_MCP_OK) return -6.0;
    double per_call = drive(channel, "example.echo");
    (void)maelys_mcp_channel_destroy(channel);
    (void)maelys_mcp_runtime_destroy(runtime);
    return per_call;
}

static double measure_in_process(void) {
    maelys_mcp_tool_t tool = {
        .name = "perf.noop",
        .title = "No-op",
        .description = "Returns a constant, to isolate dispatch cost from IPC.",
        .effect = MAELYS_MCP_EFFECT_READ
    };
    tool.input_schema = json_pack("{s:s}", "type", "object");
    if (!tool.input_schema) return -1.0;
    maelys_mcp_provider_config_t config = {
        .name = "perf-inproc", .version = "1",
        .tools = &tool, .tool_count = 1,
        .call = inproc_call
    };
    maelys_mcp_provider_t *provider = NULL;
    maelys_mcp_result_t created = maelys_mcp_provider_create(&config, &provider);
    json_decref(tool.input_schema);
    if (created != MAELYS_MCP_OK) return -2.0;

    maelys_mcp_runtime_config_t runtime_config = {
        .server_name = "provider-perf", .server_version = "1"
    };
    maelys_mcp_runtime_t *runtime = NULL;
    if (maelys_mcp_runtime_create(&runtime_config, &runtime) != MAELYS_MCP_OK) return -3.0;
    if (maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_TOOLS) != MAELYS_MCP_OK) return -4.0;
    if (maelys_mcp_runtime_add_provider(runtime, provider, NULL) != MAELYS_MCP_OK) return -5.0;
    maelys_mcp_channel_t *channel = NULL;
    if (maelys_mcp_channel_create(runtime, NULL, &channel) != MAELYS_MCP_OK) return -6.0;
    double per_call = drive(channel, "perf.noop");
    (void)maelys_mcp_channel_destroy(channel);
    (void)maelys_mcp_runtime_destroy(runtime);
    return per_call;
}

static int test_tools_call_round_trip_cost(void) {
    double out_of_process = measure_out_of_process();
    ASSERT_TRUE(out_of_process >= 0.0);
    double in_process = measure_in_process();
    ASSERT_TRUE(in_process >= 0.0);

    fprintf(stderr, "   tools/call round trip (%d iterations each)\n", ITERATIONS);
    fprintf(stderr, "     out-of-process provider: %8.2f us  %8.0f calls/s\n",
        out_of_process, 1e6 / out_of_process);
    fprintf(stderr, "     in-process provider    : %8.2f us  %8.0f calls/s\n",
        in_process, 1e6 / in_process);
    fprintf(stderr, "     cost of IPC isolation  : %8.2f us  (%.0f%% of the round trip)\n",
        out_of_process - in_process,
        100.0 * (out_of_process - in_process) / out_of_process);

    ASSERT_TRUE(out_of_process < CEILING_US);
    ASSERT_TRUE(in_process < CEILING_US);
    /* Structural, not timing-based: crossing the process boundary cannot be
     * cheaper than staying in-process. If this ever inverts, the measurement
     * is not measuring what it claims to. */
    ASSERT_TRUE(out_of_process > in_process);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/example-provider\n", argv[0]);
        return 1;
    }
    provider_path = argv[1];
    static const maelys_test_case_t tests[] = {
        {"tools/call round trip cost, in and out of process",
            test_tools_call_round_trip_cost}
    };
    return maelys_run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
