#pragma once

#include <stddef.h>

#include "maelys/mcp/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_mcp_provider_sdk maelys_mcp_provider_sdk_t;

typedef maelys_mcp_result_t (*maelys_mcp_provider_sdk_call_fn)(
    maelys_mcp_provider_sdk_t *sdk,
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error);

typedef maelys_mcp_result_t (*maelys_mcp_provider_sdk_read_resource_fn)(
    maelys_mcp_provider_sdk_t *sdk,
    void *context,
    const maelys_mcp_resource_request_t *request,
    maelys_mcp_resource_result_t *out_result,
    char **out_error);

typedef maelys_mcp_result_t (*maelys_mcp_provider_sdk_activate_fn)(
    maelys_mcp_provider_sdk_t *sdk,
    void *context,
    char **out_error);

/* Called once when serve() starts shutting down. A provider that created event
 * producer threads must stop and join them here before returning. */
typedef void (*maelys_mcp_provider_sdk_shutdown_fn)(void *context);

typedef void (*maelys_mcp_provider_sdk_destroy_fn)(void *context);

typedef struct maelys_mcp_provider_sdk_config {
    const char *name;
    const char *version;
    const maelys_mcp_tool_t *tools;
    size_t tool_count;
    const maelys_mcp_resource_t *resources;
    size_t resource_count;
    const maelys_mcp_resource_template_t *resource_templates;
    size_t resource_template_count;
    maelys_mcp_provider_sdk_call_fn call;
    maelys_mcp_provider_sdk_read_resource_fn read_resource;
    maelys_mcp_provider_sdk_activate_fn activate;
    maelys_mcp_provider_sdk_shutdown_fn shutdown;
    maelys_mcp_provider_sdk_destroy_fn destroy;
    void *context;
} maelys_mcp_provider_sdk_config_t;

typedef struct maelys_mcp_provider_sdk_options {
    size_t max_message_bytes;
    /*
     * <= 0 defers to MAELYS_PROVIDER_FD when it is set and usable (the
     * MAELYS_MCP_PROCESS_FD_ISOLATED launch layout - see
     * docs/launch-contract-design.md), and to STDIN_FILENO/STDOUT_FILENO or
     * the isolated stdout fd otherwise. A positive value here always wins
     * over the environment: it is a more deliberate statement than an
     * inherited variable.
     */
    int input_fd;       /* <= 0 means STDIN_FILENO. */
    int output_fd;      /* <= 0 means STDOUT_FILENO, or the isolated stdout fd. */
    /* Non-zero leaves stdout untouched when output_fd uses STDOUT_FILENO.
     * Zero is the safe default: protocol writes use a duplicated stdout fd and
     * application stdout is redirected to stderr. Never consulted when
     * MAELYS_PROVIDER_FD supplies the descriptor instead - the host has
     * already isolated fd 1 at the kernel level in that layout, so there is
     * nothing left in this process's own stdout to protect. */
    int disable_stdout_isolation;
} maelys_mcp_provider_sdk_options_t;

/* The config and every descriptor it references remain caller-owned and must
 * stay valid until serve() returns. A callback transfers ownership of every
 * JSON value it assigns to out_result; callback errors must be
 * malloc-compatible allocations.
 *
 * emit_event() is thread-safe after activation. It may be called from provider
 * worker threads only while serve() is running. The shutdown callback must stop
 * and join every such worker before returning; after shutdown begins, new event
 * emissions are denied. */
maelys_mcp_result_t maelys_mcp_provider_sdk_serve(
    const maelys_mcp_provider_sdk_config_t *config,
    const maelys_mcp_provider_sdk_options_t *options);

/*
 * Reports progress for the call currently being handled. Unlike an event,
 * this is request-scoped: the host routes it to the one call in flight, so
 * it must only be sent from inside a tool handler. No progress token is
 * carried - the host holds it, which is what stops a provider addressing
 * progress at a request that is not its own.
 *
 * Best effort: progress is advisory, and a host that did not ask for it (no
 * progressToken on the request) simply drops it. Pass a negative `total` to
 * omit it, and NULL `message` to omit that.
 */
maelys_mcp_result_t maelys_mcp_provider_sdk_report_progress(
    maelys_mcp_provider_sdk_t *sdk,
    double progress,
    double total,
    const char *message);

maelys_mcp_result_t maelys_mcp_provider_sdk_emit_event(
    maelys_mcp_provider_sdk_t *sdk,
    const maelys_mcp_provider_event_t *event);

/*
 * Opens one request back at the client in the middle of the call currently
 * being handled and blocks until the client answers - MCP's older, nested
 * multi-round-trip pattern (docs/provider-protocol.md, `maelys-provider/5`),
 * the counterpart of the `input_required` result rather than a replacement
 * for it. Must be called only from inside a `call`/`read_resource` callback,
 * on the thread serve() is running on: the SDK's serve loop is single
 * threaded, so this is a plain blocking call, not a handoff to a second
 * thread, and the host guarantees the correlated reply is the next thing on
 * the wire.
 *
 * `method` must be one of "elicitation/create", "sampling/createMessage" or
 * "roots/list"; anything else - and any method whose capability the client
 * never declared - is refused by the host with `denied` before a byte
 * reaches the client, which this surfaces as MAELYS_MCP_ERR_DENIED. `params`
 * is borrowed and may be NULL. On MAELYS_MCP_OK, `*out_result` holds the
 * client's result, owned by the caller.
 *
 * On failure `*out_error` is set (malloc-owned) and the result mirrors the
 * host's wire codes: MAELYS_MCP_ERR_DENIED ("denied"), MAELYS_MCP_ERR_TIMEOUT
 * ("timeout"), MAELYS_MCP_ERR_CLOSED ("cancelled" - the outer call was
 * cancelled, or the connection went away), MAELYS_MCP_ERR_STATE
 * ("unavailable" - this call cannot nest), or MAELYS_MCP_ERR_PROVIDER for
 * everything else, including "client_error" (a JSON-RPC error from the
 * client, which travels back in `*out_result`, mirroring the in-process
 * maelys_mcp_provider_request_client contract) and "failed". A malformed or
 * out-of-order reply from the host is reported as MAELYS_MCP_ERR_PROTOCOL.
 *
 * Only one nested request may be outstanding per call, matching the wire's
 * own single-outstanding rule; calling this again before the first returns
 * fails with MAELYS_MCP_ERR_STATE rather than corrupting the connection.
 *
 * A session declares `maelys-provider/4` until this is called for the first
 * time; from the nestedRequest frame that call writes onward, it declares
 * `/5` - raised, never lowered. A provider that never calls this is
 * therefore byte-for-byte unchanged from before this function existed: no
 * opt-in flag is needed, and declaring /5 unconditionally would have cost
 * every non-nesting provider its compatibility with a host that predates the
 * nested-requests version-range fix, for a capability it never uses.
 */
maelys_mcp_result_t maelys_mcp_provider_sdk_request_client(
    maelys_mcp_provider_sdk_t *sdk,
    const char *method,
    json_t *params,
    json_t **out_result,
    char **out_error);

#ifdef __cplusplus
}
#endif
