#include "maelys/mcp.h"
#include "src/internal/internal.h"
#include "host/http_auth.h"
#include "host/http_server.h"
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
        "                  [--http-listen ADDRESS:PORT] [--http-path /mcp]\n"
        "                  [--http-allow-origin ORIGIN ...]\n"
        "                  [--http-bearer-token TOKEN ...]\n"
        "\n"
        "At least one --provider or a --manifest with at least one provider is\n"
        "required. --manifest (docs/manifest.md) declares a whole provider set,\n"
        "including federated mcp-proxy providers that --provider alone cannot;\n"
        "its providers are added after --provider's, in the manifest's order.\n"
        "Its \"allowEffects\" is OR-ed with --allow-effect rather than replacing it.\n"
        "\n"
        "--http-listen starts the HTTP listener alongside stdio. It binds\n"
        "127.0.0.1 unless told otherwise, and a non-loopback address refuses to\n"
        "start unless --http-bearer-token supplies an authenticator other than\n"
        "loopback trust. THE HTTP ENDPOINT DOES NOT SERVE MCP YET: it parses,\n"
        "routes and authenticates, and answers 503 (see docs/protocol-support.md).\n");
}

/*
 * ADDRESS:PORT, where ADDRESS may be an IPv6 literal in brackets. Split on the
 * LAST colon so that "[::1]:8080" and "127.0.0.1:8080" both work and an
 * unbracketed IPv6 literal - which has several colons and no unambiguous port -
 * is refused by inet_pton rather than guessed at.
 */
static int parse_listen(const char *value, char *address, size_t capacity,
    unsigned short *out_port) {
    if (!value || !*value) return -1;
    const char *colon = strrchr(value, ':');
    if (!colon || colon == value) return -1;
    size_t address_length = (size_t)(colon - value);
    if (address_length >= capacity) return -1;
    if (value[0] == '[') {
        if (colon[-1] != ']' || address_length < 3u) return -1;
        memcpy(address, value + 1u, address_length - 2u);
        address[address_length - 2u] = '\0';
    } else {
        memcpy(address, value, address_length);
        address[address_length] = '\0';
    }
    errno = 0;
    char *end = NULL;
    unsigned long port = strtoul(colon + 1, &end, 10);
    if (errno || !end || *end || port > 65535u) return -1;
    *out_port = (unsigned short)port;
    return 0;
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

/* Fixed caps rather than a second heap allocation on every early-return path.
 * Both are far past any real configuration, and a configuration that needs
 * more origins than this is one an allowlist is the wrong shape for. */
#define MAX_HTTP_ORIGINS 64u
#define MAX_HTTP_TOKENS 64u

int main(int argc, char **argv) {
    const char **provider_paths = calloc((size_t)argc, sizeof(*provider_paths));
    if (!provider_paths) return 1;
    size_t provider_count = 0;
    char http_address[64] = {0};
    unsigned short http_port = 0;
    int http_enabled = 0;
    const char *http_path = NULL;
    const char *http_origins[MAX_HTTP_ORIGINS];
    size_t http_origin_count = 0;
    const char *http_tokens[MAX_HTTP_TOKENS];
    size_t http_token_count = 0;
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
        } else if (strcmp(argv[index], "--http-listen") == 0 && index + 1 < argc &&
            parse_listen(argv[++index], http_address, sizeof(http_address),
                &http_port) == 0) {
            http_enabled = 1;
        } else if (strcmp(argv[index], "--http-path") == 0 && index + 1 < argc &&
            argv[index + 1][0] == '/') {
            http_path = argv[++index];
        } else if (strcmp(argv[index], "--http-allow-origin") == 0 && index + 1 < argc &&
            http_origin_count < MAX_HTTP_ORIGINS) {
            http_origins[http_origin_count++] = argv[++index];
        } else if (strcmp(argv[index], "--http-bearer-token") == 0 && index + 1 < argc &&
            http_token_count < MAX_HTTP_TOKENS) {
            http_tokens[http_token_count++] = argv[++index];
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

    /*
     * The HTTP listener, when asked for. It runs alongside stdio ON THE SAME
     * RUNTIME, and since H3 it serves MCP: each POST creates a channel bound to
     * that request's authenticated principal and to the modern era alone, so
     * one process answers 2026-07-28 over HTTP and every era over stdio without
     * either transport rewriting the other's dispatch results.
     */
    maelys_mcp_authenticator_t authenticator = {0};
    maelys_mcp_http_adapter_t *adapter = NULL;
    maelys_http_server_t *http_server = NULL;
    if (http_enabled) {
        maelys_mcp_result_t http_status = http_token_count
            ? maelys_http_auth_static_bearer_create(http_tokens, http_token_count,
                &authenticator)
            : maelys_http_auth_loopback_create(&authenticator);
        if (http_status == MAELYS_MCP_OK) {
            maelys_mcp_http_adapter_config_t adapter_config = {
                .runtime = runtime,
                /* Every other field zero, which is each one's own way of saying
                 * "the runtime's default": the same channel budgets stdio gets,
                 * the 15 s keep-alive, and the 5 s close deadline. */
                .max_bytes = 0u
            };
            http_status = maelys_mcp_http_adapter_create(&adapter_config, &adapter);
        }
        if (http_status == MAELYS_MCP_OK) {
            maelys_http_server_options_t http_options = {
                .bind_address = http_address,
                .port = http_port,
                .endpoint_path = http_path,
                .allowed_origins = http_origins,
                .allowed_origin_count = http_origin_count,
                .max_body_bytes = config.max_message_bytes,
                .write_timeout_ms = stdio_write_timeout_ms,
                .authenticator = &authenticator,
                .adapter = adapter
            };
            http_status = maelys_http_server_start(&http_options, &http_server);
        }
        if (http_status != MAELYS_MCP_OK) {
            fprintf(stderr, "Cannot start the HTTP listener on %s:%u: %s\n",
                http_address, (unsigned int)http_port,
                maelys_mcp_result_string(http_status));
            if (adapter) maelys_mcp_http_adapter_destroy(adapter);
            if (authenticator.destroy) authenticator.destroy(authenticator.context);
            maelys_mcp_result_t destroy_status = maelys_mcp_runtime_destroy(runtime);
            if (destroy_status != MAELYS_MCP_OK) {
                fprintf(stderr, "Cannot destroy runtime: %s\n",
                    maelys_mcp_result_string(destroy_status));
            }
            close(transport_fd);
            return 1;
        }
    }

    maelys_mcp_stdio_options_t stdio_options = {
        .write_timeout_ms = stdio_write_timeout_ms
    };
    status = maelys_mcp_runtime_serve_stdio_with_options(
        runtime, STDIN_FILENO, transport_fd, &stdio_options);
    /*
     * Teardown order, and every step of it is load-bearing now that a
     * connection holds a channel and a channel holds a principal.
     *
     * Stop accepting and join every connection first, so no new channel can be
     * created. Destroy the runtime SECOND, because a channel that missed its
     * close deadline was detached rather than waited on and is still out there:
     * maelys_mcp_runtime_destroy drains those, which is what runs the last
     * context_release and hands the last principal reference back. Only then
     * destroy the authenticator, whose own contract is that it is destroyed
     * "after the last channel that could have carried one of its principals is
     * destroyed" - doing it before the runtime would be releasing a principal
     * into an authenticator that had already freed its table.
     */
    if (http_server) (void)maelys_http_server_stop(http_server);
    if (adapter) maelys_mcp_http_adapter_destroy(adapter);
    maelys_mcp_result_t destroy_status = maelys_mcp_runtime_destroy(runtime);
    if (authenticator.destroy) authenticator.destroy(authenticator.context);
    if (status == MAELYS_MCP_OK) status = destroy_status;
    close(transport_fd);
    if (status != MAELYS_MCP_OK) {
        fprintf(stderr, "MCP transport failed: %s\n", maelys_mcp_result_string(status));
        return 1;
    }
    return 0;
}
