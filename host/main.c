#include "maelys/mcp.h"
#include "src/internal/internal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef MAELYS_MCP_VERSION
#define MAELYS_MCP_VERSION "0.0.0-dev"
#endif

typedef struct host_policy {
    unsigned int allowed_effects;
} host_policy_t;

static int authorize(void *context, const maelys_mcp_request_context_t *request) {
    const host_policy_t *policy = context;
    return request->effect == MAELYS_MCP_EFFECT_READ ||
        request->effect == MAELYS_MCP_EFFECT_PREVIEW ||
        (policy->allowed_effects & (1u << (unsigned int)request->effect)) != 0;
}

static void usage(FILE *stream) {
    fprintf(stream,
        "Usage: maelys-mcp --provider /absolute/path [--provider /absolute/path ...]\n"
        "                  [--allow-effect apply|commit|execute ...]\n"
        "                  [--provider-describe-timeout-ms N]\n"
        "                  [--provider-call-timeout-ms N]\n"
        "                  [--provider-shutdown-timeout-ms N]\n");
}

static int parse_timeout(const char *value, unsigned int *out) {
    if (!value || !*value || !out || *value == '-') return -1;
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno || !end || *end || parsed == 0 || parsed > UINT_MAX) return -1;
    *out = (unsigned int)parsed;
    return 0;
}

int main(int argc, char **argv) {
    const char **provider_paths = calloc((size_t)argc, sizeof(*provider_paths));
    if (!provider_paths) return 1;
    size_t provider_count = 0;
    host_policy_t policy = {0};
    unsigned int describe_timeout_ms = MAELYS_MCP_DEFAULT_PROVIDER_DESCRIBE_TIMEOUT_MS;
    unsigned int call_timeout_ms = MAELYS_MCP_DEFAULT_PROVIDER_CALL_TIMEOUT_MS;
    unsigned int shutdown_timeout_ms = MAELYS_MCP_DEFAULT_PROVIDER_SHUTDOWN_TIMEOUT_MS;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--provider") == 0 && index + 1 < argc) {
            provider_paths[provider_count++] = argv[++index];
        } else if (strcmp(argv[index], "--allow-effect") == 0 && index + 1 < argc) {
            maelys_mcp_tool_effect_t effect = MAELYS_MCP_EFFECT_UNSPECIFIED;
            if (maelys_mcp_tool_effect_parse(argv[++index], &effect) != MAELYS_MCP_OK ||
                effect == MAELYS_MCP_EFFECT_READ || effect == MAELYS_MCP_EFFECT_PREVIEW) {
                fprintf(stderr, "--allow-effect accepts apply, commit, or execute.\n");
                free(provider_paths);
                return 2;
            }
            policy.allowed_effects |= 1u << (unsigned int)effect;
        } else if (strcmp(argv[index], "--provider-describe-timeout-ms") == 0 &&
            index + 1 < argc && parse_timeout(argv[++index], &describe_timeout_ms) == 0) {
        } else if (strcmp(argv[index], "--provider-call-timeout-ms") == 0 &&
            index + 1 < argc && parse_timeout(argv[++index], &call_timeout_ms) == 0) {
        } else if (strcmp(argv[index], "--provider-shutdown-timeout-ms") == 0 &&
            index + 1 < argc && parse_timeout(argv[++index], &shutdown_timeout_ms) == 0) {
        } else if (strcmp(argv[index], "--help") == 0) {
            usage(stdout);
            free(provider_paths);
            return 0;
        } else {
            usage(stderr);
            free(provider_paths);
            return 2;
        }
    }
    if (provider_count == 0) {
        fprintf(stderr, "At least one --provider is required.\n");
        usage(stderr);
        free(provider_paths);
        return 2;
    }

    int transport_fd = -1;
    maelys_mcp_result_t status = maelys_mcp_isolate_stdout(&transport_fd);
    if (status != MAELYS_MCP_OK) {
        fprintf(stderr, "Cannot isolate MCP stdout: %s\n", maelys_mcp_result_string(status));
        free(provider_paths);
        return 1;
    }

    maelys_mcp_runtime_config_t config = {
        .server_name = "maelys-mcp",
        .server_version = MAELYS_MCP_VERSION,
        .instructions = "A policy-enforced local MCP runtime for explicitly configured providers.",
        .max_providers = provider_count,
        .max_message_bytes = MAELYS_MCP_DEFAULT_MAX_MESSAGE_BYTES,
        .authorize = authorize,
        .policy_context = &policy
    };
    maelys_mcp_runtime_t *runtime = NULL;
    status = maelys_mcp_runtime_create(&config, &runtime);
    if (status != MAELYS_MCP_OK) {
        fprintf(stderr, "Cannot create runtime: %s\n", maelys_mcp_result_string(status));
        close(transport_fd);
        free(provider_paths);
        return 1;
    }
    status = maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_TOOLS);
    if (status == MAELYS_MCP_OK) {
        status = maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_MRTR);
    }
    if (status == MAELYS_MCP_OK) {
        status = maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_RESOURCES);
    }
    if (status != MAELYS_MCP_OK) {
        fprintf(stderr, "Cannot enable MCP modules: %s\n",
            maelys_mcp_result_string(status));
        maelys_mcp_runtime_destroy(runtime);
        close(transport_fd);
        free(provider_paths);
        return 1;
    }
    for (size_t index = 0; index < provider_count; ++index) {
        maelys_mcp_provider_t *provider = NULL;
        char *error = NULL;
        maelys_mcp_provider_process_options_t options = {
            .executable_path = provider_paths[index],
            .max_message_bytes = config.max_message_bytes,
            .describe_timeout_ms = describe_timeout_ms,
            .call_timeout_ms = call_timeout_ms,
            .shutdown_timeout_ms = shutdown_timeout_ms
        };
        status = maelys_mcp_provider_spawn_with_options(
            &options, &provider, &error);
        if (status == MAELYS_MCP_OK) {
            status = maelys_mcp_runtime_add_provider(runtime, provider, &error);
        }
        if (status != MAELYS_MCP_OK) {
            fprintf(stderr, "Cannot load provider %s: %s%s%s\n",
                provider_paths[index], maelys_mcp_result_string(status),
                error ? ": " : "", error ? error : "");
            if (provider) maelys_mcp_provider_destroy(provider);
            free(error);
            maelys_mcp_runtime_destroy(runtime);
            close(transport_fd);
            free(provider_paths);
            return 1;
        }
        free(error);
    }
    free(provider_paths);
    status = maelys_mcp_runtime_serve_stdio(runtime, STDIN_FILENO, transport_fd);
    maelys_mcp_runtime_destroy(runtime);
    close(transport_fd);
    if (status != MAELYS_MCP_OK) {
        fprintf(stderr, "MCP transport failed: %s\n", maelys_mcp_result_string(status));
        return 1;
    }
    return 0;
}
