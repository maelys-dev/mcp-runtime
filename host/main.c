#include "maelys/mcp.h"
#include "src/internal/internal.h"
#include "host/manifest.h"

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

/*
 * The reference host's whole policy: an effect allowlist, expressed as one
 * middleware implementing hook 2. It reads nothing per-channel, so it never
 * looks at maelys_mcp_channel_context - a host that authenticates its clients
 * would decide on that pointer instead, never on request->client_name, which
 * is client-asserted.
 */
static maelys_mcp_authorize_decision_t authorize_effect(
    void *context,
    const maelys_mcp_authorize_context_t *request) {
    const host_policy_t *policy = context;
    int allowed = request->effect == MAELYS_MCP_EFFECT_READ ||
        request->effect == MAELYS_MCP_EFFECT_PREVIEW ||
        (policy->allowed_effects & (1u << (unsigned int)request->effect)) != 0;
    return allowed ? MAELYS_MCP_AUTHORIZE_ALLOW : MAELYS_MCP_AUTHORIZE_DENY;
}

static void usage(FILE *stream) {
    fprintf(stream,
        "Usage: maelys-mcp [--provider /absolute/path ...] [--manifest /absolute/path]\n"
        "                  [--allow-effect apply|commit|execute ...]\n"
        "                  [--provider-describe-timeout-ms N]\n"
        "                  [--provider-call-timeout-ms N]\n"
        "                  [--provider-shutdown-timeout-ms N]\n"
        "                  [--stdio-write-timeout-ms N]\n"
        "\n"
        "At least one --provider or a --manifest with at least one provider is\n"
        "required. --manifest (docs/manifest.md) declares a whole provider set,\n"
        "including federated mcp-proxy providers that --provider alone cannot;\n"
        "its providers are added after --provider's, in the manifest's order.\n"
        "Its \"allowEffects\" is OR-ed with --allow-effect rather than replacing it.\n");
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
    const char *manifest_path = NULL;
    unsigned int describe_timeout_ms = MAELYS_MCP_DEFAULT_PROVIDER_DESCRIBE_TIMEOUT_MS;
    unsigned int call_timeout_ms = MAELYS_MCP_DEFAULT_PROVIDER_CALL_TIMEOUT_MS;
    unsigned int shutdown_timeout_ms = MAELYS_MCP_DEFAULT_PROVIDER_SHUTDOWN_TIMEOUT_MS;
    unsigned int stdio_write_timeout_ms = MAELYS_MCP_DEFAULT_STDIO_WRITE_TIMEOUT_MS;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--provider") == 0 && index + 1 < argc) {
            provider_paths[provider_count++] = argv[++index];
        } else if (strcmp(argv[index], "--manifest") == 0 && index + 1 < argc) {
            manifest_path = argv[++index];
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
        } else if (strcmp(argv[index], "--stdio-write-timeout-ms") == 0 &&
            index + 1 < argc && parse_timeout(argv[++index], &stdio_write_timeout_ms) == 0) {
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

    manifest_t manifest = {0};
    if (manifest_path) {
        char *error = NULL;
        maelys_mcp_result_t manifest_status = manifest_load(manifest_path, &manifest, &error);
        if (manifest_status != MAELYS_MCP_OK) {
            fprintf(stderr, "Cannot load manifest %s: %s%s%s\n",
                manifest_path, maelys_mcp_result_string(manifest_status),
                error ? ": " : "", error ? error : "");
            free(error);
            free(provider_paths);
            return 2;
        }
        free(error);
        /* --allow-effect and the manifest's allowEffects are additive, never
         * one replacing the other - see the usage text above. */
        policy.allowed_effects |= manifest.allowed_effects;
    }
    if (provider_count == 0 && manifest.provider_count == 0) {
        fprintf(stderr, "At least one --provider or a --manifest provider is required.\n");
        usage(stderr);
        manifest_clear(&manifest);
        free(provider_paths);
        return 2;
    }

    int transport_fd = -1;
    maelys_mcp_result_t status = maelys_mcp_isolate_stdout(&transport_fd);
    if (status != MAELYS_MCP_OK) {
        fprintf(stderr, "Cannot isolate MCP stdout: %s\n", maelys_mcp_result_string(status));
        manifest_clear(&manifest);
        free(provider_paths);
        return 1;
    }

    maelys_mcp_runtime_config_t config = {
        .server_name = "maelys-mcp",
        .server_version = MAELYS_MCP_VERSION,
        .instructions = "A policy-enforced local MCP runtime for explicitly configured providers.",
        .max_providers = provider_count + manifest.provider_count,
        .max_message_bytes = MAELYS_MCP_DEFAULT_MAX_MESSAGE_BYTES
    };
    maelys_mcp_runtime_t *runtime = NULL;
    status = maelys_mcp_runtime_create(&config, &runtime);
    if (status != MAELYS_MCP_OK) {
        fprintf(stderr, "Cannot create runtime: %s\n", maelys_mcp_result_string(status));
        close(transport_fd);
        manifest_clear(&manifest);
        free(provider_paths);
        return 1;
    }
    maelys_mcp_middleware_t effect_policy = {
        .name = "host-effect-allowlist",
        .context = &policy,
        .on_authorize = authorize_effect
    };
    status = maelys_mcp_runtime_add_middleware(runtime, &effect_policy, NULL);
    if (status == MAELYS_MCP_OK) {
        status = maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_TOOLS);
    }
    if (status == MAELYS_MCP_OK) {
        status = maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_MRTR);
    }
    if (status == MAELYS_MCP_OK) {
        status = maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_RESOURCES);
    }
    if (status == MAELYS_MCP_OK) {
        status = maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_SUBSCRIPTIONS);
    }
    if (status != MAELYS_MCP_OK) {
        fprintf(stderr, "Cannot configure the MCP runtime: %s\n",
            maelys_mcp_result_string(status));
        maelys_mcp_result_t destroy_status = maelys_mcp_runtime_destroy(runtime);
        if (destroy_status != MAELYS_MCP_OK) {
            fprintf(stderr, "Cannot destroy runtime: %s\n",
                maelys_mcp_result_string(destroy_status));
        }
        close(transport_fd);
        manifest_clear(&manifest);
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
            maelys_mcp_result_t destroy_status = maelys_mcp_runtime_destroy(runtime);
            if (destroy_status != MAELYS_MCP_OK) {
                fprintf(stderr, "Cannot destroy runtime: %s\n",
                    maelys_mcp_result_string(destroy_status));
            }
            close(transport_fd);
            manifest_clear(&manifest);
            free(provider_paths);
            return 1;
        }
        free(error);
    }
    free(provider_paths);
    /* Manifest providers are added after --provider's, in the manifest's own
     * order - see the usage text. */
    for (size_t index = 0; index < manifest.provider_count; ++index) {
        const manifest_provider_t *entry = &manifest.providers[index];
        maelys_mcp_provider_t *provider = NULL;
        char *error = NULL;
        char *skipped_tools = NULL;
        if (entry->type == MANIFEST_PROVIDER_NATIVE) {
            maelys_mcp_provider_process_options_t options = {
                .executable_path = entry->path,
                .max_message_bytes = entry->max_message_bytes,
                .describe_timeout_ms = entry->describe_timeout_ms,
                .call_timeout_ms = entry->call_timeout_ms,
                .shutdown_timeout_ms = entry->shutdown_timeout_ms
            };
            /*
             * Always the v2-capable entry point: a v1 manifest (or a v2 one
             * that set neither key) has entry->args == NULL and
             * entry->execution_profile == NULL, which
             * maelys_mcp_provider_spawn_with_args defines as byte-identical
             * to maelys_mcp_provider_spawn_with_options - so there is nothing
             * for a manifestVersion branch here to do.
             */
            status = maelys_mcp_provider_spawn_with_args(
                &options, entry->args, entry->execution_profile, &provider, &error);
        } else {
            maelys_mcp_proxy_options_t options = {
                .executable_path = entry->path,
                .argv = entry->argv,
                .max_message_bytes = entry->max_message_bytes,
                .connect_timeout_ms = entry->connect_timeout_ms,
                .call_timeout_ms = entry->call_timeout_ms,
                .default_effect = entry->default_effect,
                .tool_prefix = entry->tool_prefix,
                .schema_policy = entry->schema_policy
            };
            /* Same reasoning as the native branch above, for
             * executionProfile. */
            status = maelys_mcp_provider_proxy_spawn_with_profile(
                &options, entry->execution_profile, &provider, &skipped_tools, &error);
        }
        if (status == MAELYS_MCP_OK) {
            status = maelys_mcp_runtime_add_provider(runtime, provider, &error);
        }
        if (status != MAELYS_MCP_OK) {
            fprintf(stderr, "Cannot load manifest provider %s: %s%s%s\n",
                entry->path, maelys_mcp_result_string(status),
                error ? ": " : "", error ? error : "");
            if (provider) maelys_mcp_provider_destroy(provider);
            free(error);
            free(skipped_tools);
            maelys_mcp_result_t destroy_status = maelys_mcp_runtime_destroy(runtime);
            if (destroy_status != MAELYS_MCP_OK) {
                fprintf(stderr, "Cannot destroy runtime: %s\n",
                    maelys_mcp_result_string(destroy_status));
            }
            close(transport_fd);
            manifest_clear(&manifest);
            return 1;
        }
        free(error);
        /* Guarded so a clean run keeps stderr empty (scripts/test_stdio.sh
         * asserts that): only printed when a tool was actually skipped. */
        if (skipped_tools) {
            fprintf(stderr, "Provider %s skipped tools with unsupported schemas: %s\n",
                entry->path, skipped_tools);
            free(skipped_tools);
        }
    }
    manifest_clear(&manifest);
    maelys_mcp_stdio_options_t stdio_options = {
        .write_timeout_ms = stdio_write_timeout_ms
    };
    status = maelys_mcp_runtime_serve_stdio_with_options(
        runtime, STDIN_FILENO, transport_fd, &stdio_options);
    maelys_mcp_result_t destroy_status = maelys_mcp_runtime_destroy(runtime);
    if (status == MAELYS_MCP_OK) status = destroy_status;
    close(transport_fd);
    if (status != MAELYS_MCP_OK) {
        fprintf(stderr, "MCP transport failed: %s\n", maelys_mcp_result_string(status));
        return 1;
    }
    return 0;
}
