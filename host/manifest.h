#pragma once

#include <stddef.h>

#include "maelys/mcp.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The host's v1 provider manifest (docs/manifest.md): a JSON file loaded
 * through --manifest that declares the whole provider set in one place,
 * instead of one --provider flag per native provider on the command line.
 *
 * Deliberately host-level, not part of the public library: manifest.h and
 * manifest.c compile into the maelys-mcp binary only (see the Makefile), and
 * nothing in include/ changes because of this file.
 */

typedef enum manifest_provider_type {
    MANIFEST_PROVIDER_NATIVE = 0,
    MANIFEST_PROVIDER_MCP_PROXY = 1
} manifest_provider_type_t;

typedef struct manifest_provider {
    manifest_provider_type_t type;
    /* Absolute, already validated. */
    char *path;

    /* mcp-proxy only. NULL-terminated; NULL (argv_count 0) when the manifest
     * did not set "argv", which means {path} to maelys_mcp_proxy_options_t. */
    char **argv;
    size_t argv_count;
    /* mcp-proxy only. NULL when the manifest did not set "toolPrefix". */
    char *tool_prefix;
    /* mcp-proxy only. MAELYS_MCP_EFFECT_UNSPECIFIED when the manifest did not
     * set "defaultEffect", which maelys_mcp_provider_proxy_spawn treats as
     * EXECUTE. */
    maelys_mcp_tool_effect_t default_effect;
    /* mcp-proxy only. MAELYS_MCP_PROXY_SCHEMA_STRICT (the default) when the
     * manifest did not set "schemaPolicy". */
    maelys_mcp_proxy_schema_policy_t schema_policy;
    /* mcp-proxy only. 0 (absent) keeps maelys_mcp_provider_proxy_spawn's own
     * default. */
    unsigned int connect_timeout_ms;

    /*
     * native only. manifestVersion 2 only ("args", docs/manifest.md): EXTRA
     * argv entries only, never the complete vector - the runtime always
     * compiles argv[0] = path, argv[1] = "--provider", then these
     * (docs/launch-contract-design.md, "Two layers of argv"). NULL-terminated;
     * NULL (args_count 0) when the manifest did not set "args", which is the
     * same thing as an empty array.
     */
    char **args;
    size_t args_count;

    /* native only. 0 (absent) keeps maelys_mcp_provider_spawn_with_options's
     * own defaults. */
    unsigned int describe_timeout_ms;
    unsigned int shutdown_timeout_ms;

    /*
     * Both types. manifestVersion 2 only ("executionProfile",
     * docs/manifest.md): opaque pass-through to the launch seam, read back by
     * a launcher through maelys_mcp_process_request_execution_profile and
     * never interpreted by the host. NULL when the manifest did not set it -
     * distinct from the string
     * "trusted-local", which is an explicit request the stock POSIX launcher
     * happens to satisfy.
     */
    char *execution_profile;

    /* Both types. 0 (absent) keeps the relevant spawn call's own default. */
    unsigned int call_timeout_ms;
    size_t max_message_bytes;
} manifest_provider_t;

typedef struct manifest {
    /* 1 or 2 (docs/manifest.md). */
    int version;
    manifest_provider_t *providers;
    size_t provider_count;
    /* Bitmask in the same encoding host/main.c's host_policy_t uses:
     * 1u << effect for each effect named in "allowEffects". Zero when the
     * manifest did not set "allowEffects". */
    unsigned int allowed_effects;
} manifest_t;

/*
 * Loads and validates the manifest at `path` (must be absolute, like every
 * other path this host takes). Two-phase: JSON syntax first, then strict
 * semantic validation - unknown keys, wrong types, an out-of-range value -
 * each rejected with an error naming the offending key and its location
 * (e.g. "providers[1].path"). Nothing is constructed from a manifest that
 * does not fully validate: *out_manifest is left zeroed on any failure.
 *
 * manifestVersion selects the key table: "args" and "executionProfile" exist
 * only under manifestVersion 2 and are unknown keys - rejected by name, like
 * any other unknown key - under manifestVersion 1.
 *
 * On success, *out_manifest is populated and must be released with
 * manifest_clear. *out_error is untouched on success and, on failure, is set
 * to a caller-owned (free() it) message; either pointer may be NULL.
 */
maelys_mcp_result_t manifest_load(
    const char *path,
    manifest_t *out_manifest,
    char **out_error);

void manifest_clear(manifest_t *manifest);

#ifdef __cplusplus
}
#endif
