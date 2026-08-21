#pragma once

#include <stddef.h>
#include <jansson.h>

#include "maelys/mcp/channel.h"
#include "maelys/mcp/error.h"
#include "maelys/mcp/module.h"
#include "maelys/mcp/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAELYS_MCP_PROTOCOL_MODERN "2026-07-28"
#define MAELYS_MCP_PROTOCOL_LEGACY "2025-11-25"
#define MAELYS_MCP_DEFAULT_MAX_MESSAGE_BYTES (1024u * 1024u)
#define MAELYS_MCP_DEFAULT_STDIO_WRITE_TIMEOUT_MS 5000u
/*
 * Ten minutes, deliberately longer than the 300-second provider call deadline:
 * the thing on the other end of a nested `elicitation/create` is a person, and
 * a deadline sized for a machine would turn "the user went to read the diff"
 * into a protocol failure. The call deadline is suspended while a nested
 * request is outstanding rather than racing it.
 */
#define MAELYS_MCP_DEFAULT_NESTED_REQUEST_TIMEOUT_MS 600000u
#define MAELYS_MCP_DEFAULT_MAX_CONCURRENT_REQUESTS 8u
#define MAELYS_MCP_DEFAULT_MAX_NESTED_REQUESTS 16u

typedef struct maelys_mcp_runtime maelys_mcp_runtime_t;

#if defined(__GNUC__) || defined(__clang__)
#define MAELYS_MCP_WARN_UNUSED_RESULT __attribute__((warn_unused_result))
#else
#define MAELYS_MCP_WARN_UNUSED_RESULT
#endif

/* The decision points a middleware's on_authorize hook is asked about. */
typedef enum maelys_mcp_operation {
    MAELYS_MCP_OPERATION_LIST = 0,
    MAELYS_MCP_OPERATION_CALL = 1,
    MAELYS_MCP_OPERATION_RESOURCE_LIST = 2,
    MAELYS_MCP_OPERATION_RESOURCE_TEMPLATE_LIST = 3,
    MAELYS_MCP_OPERATION_RESOURCE_READ = 4
} maelys_mcp_operation_t;

/*
 * Policy and audit are not configured here. They are registered on the
 * middleware chain (maelys/mcp/middleware.h), which replaced the runtime-wide
 * authorize/audit callback pair this struct carried through ABI 2;
 * maelys_mcp_runtime_add_compat_policy takes the same three values for an
 * embedder that has not yet moved to the chain's own hooks.
 */
typedef struct maelys_mcp_runtime_config {
    const char *server_name;
    const char *server_version;
    const char *instructions;
    size_t max_providers;
    size_t max_message_bytes;
    size_t max_subscriptions;
} maelys_mcp_runtime_config_t;

/*
 * How a channel handles the two methods that can block on a client round trip.
 * Bound once on the runtime, while it is cold, and inherited by every channel
 * - a new structure and a new entry point rather than new fields on
 * maelys_mcp_channel_config_t, which is released public layout
 * (docs/abi-policy.md). Every zero field selects its documented default, so
 * `{0}` is the current behaviour.
 */
typedef struct maelys_mcp_nested_config {
    /* How long a nested request may wait for the client's reply.
     * 0 selects MAELYS_MCP_DEFAULT_NESTED_REQUEST_TIMEOUT_MS. */
    unsigned int request_timeout_ms;
    /* How many `tools/call`/`resources/read` requests one channel may have in
     * flight at once. Beyond it, a new one waits for the channel's admission
     * timeout and is then refused with -32603 rather than queued behind the
     * transport reader. 0 selects MAELYS_MCP_DEFAULT_MAX_CONCURRENT_REQUESTS. */
    size_t max_concurrent_requests;
    /* How many nested requests one channel may have outstanding at once.
     * 0 selects MAELYS_MCP_DEFAULT_MAX_NESTED_REQUESTS. */
    size_t max_nested_requests;
} maelys_mcp_nested_config_t;

typedef struct maelys_mcp_stdio_options {
    unsigned int write_timeout_ms;
    /*
     * Configuration for the channel serve_stdio creates internally. NULL
     * keeps today's defaults (write_timeout_ms mirrored into
     * admission_timeout_ms and close_timeout_ms, everything else zero and
     * therefore runtime-defaulted). When non-NULL, this struct is used
     * as-is; write_timeout_ms above no longer overrides its timeout fields.
     */
    const maelys_mcp_channel_config_t *channel_config;
} maelys_mcp_stdio_options_t;

maelys_mcp_result_t maelys_mcp_runtime_create(
    const maelys_mcp_runtime_config_t *config,
    maelys_mcp_runtime_t **out_runtime);

/*
 * Closes channel-create admission, waits for admitted creators, and returns
 * MAELYS_MCP_ERR_STATE without freeing runtime while published channels live.
 * Once this call begins, callers must not start a new public operation using
 * runtime or one of its channels. Calls begun after the runtime is freed are
 * invalid C usage. Draining admitted creators may wait for provider activation
 * to reach its configured deadline.
 *
 * It also waits, without a bound, for every channel handed to
 * maelys_mcp_channel_destroy_detached to be released by its last in-flight
 * operation. Such a channel still points at this runtime while it exists, so
 * the wait is what stops the runtime being freed underneath it; the bound is
 * the provider's, which is the same bound the detach did not remove and only
 * moved off the caller's thread.
 */
MAELYS_MCP_WARN_UNUSED_RESULT
maelys_mcp_result_t maelys_mcp_runtime_destroy(maelys_mcp_runtime_t *runtime);

/*
 * Binds nested-request settings. Like middleware registration, this is only
 * accepted while the runtime is cold: every channel reads these values with no
 * lock, so they must stop changing before the first channel can dispatch.
 * Returns MAELYS_MCP_ERR_STATE once a channel exists.
 */
maelys_mcp_result_t maelys_mcp_runtime_configure_nested(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_nested_config_t *config);

maelys_mcp_result_t maelys_mcp_runtime_add_provider(
    maelys_mcp_runtime_t *runtime,
    maelys_mcp_provider_t *provider,
    char **out_error);

maelys_mcp_result_t maelys_mcp_runtime_serve_stdio(
    maelys_mcp_runtime_t *runtime,
    int read_fd,
    int write_fd);

/*
 * Serves stdio with bounded output writes. A zero timeout selects
 * MAELYS_MCP_DEFAULT_STDIO_WRITE_TIMEOUT_MS.
 */
maelys_mcp_result_t maelys_mcp_runtime_serve_stdio_with_options(
    maelys_mcp_runtime_t *runtime,
    int read_fd,
    int write_fd,
    const maelys_mcp_stdio_options_t *options);

#ifdef __cplusplus
}
#endif

#undef MAELYS_MCP_WARN_UNUSED_RESULT
