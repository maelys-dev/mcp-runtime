#include "maelys/mcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
        "                  [--allow-effect apply|commit|execute ...]\n");
}

int main(int argc, char **argv) {
    const char **provider_paths = calloc((size_t)argc, sizeof(*provider_paths));
    if (!provider_paths) return 1;
    size_t provider_count = 0;
    host_policy_t policy = {0};
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

    maelys_mcp_runtime_config_t config = {
        .server_name = "maelys-mcp",
        .server_version = "0.1.0",
        .instructions = "A policy-enforced local MCP runtime for explicitly configured providers.",
        .max_providers = provider_count,
        .max_message_bytes = MAELYS_MCP_DEFAULT_MAX_MESSAGE_BYTES,
        .authorize = authorize,
        .policy_context = &policy
    };
    maelys_mcp_runtime_t *runtime = NULL;
    maelys_mcp_result_t status = maelys_mcp_runtime_create(&config, &runtime);
    if (status != MAELYS_MCP_OK) {
        fprintf(stderr, "Cannot create runtime: %s\n", maelys_mcp_result_string(status));
        free(provider_paths);
        return 1;
    }
    for (size_t index = 0; index < provider_count; ++index) {
        maelys_mcp_provider_t *provider = NULL;
        char *error = NULL;
        status = maelys_mcp_provider_spawn(
            provider_paths[index], config.max_message_bytes, &provider, &error);
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
            free(provider_paths);
            return 1;
        }
        free(error);
    }
    free(provider_paths);
    status = maelys_mcp_runtime_serve_stdio(runtime, STDIN_FILENO, STDOUT_FILENO);
    maelys_mcp_runtime_destroy(runtime);
    if (status != MAELYS_MCP_OK) {
        fprintf(stderr, "MCP transport failed: %s\n", maelys_mcp_result_string(status));
        return 1;
    }
    return 0;
}
