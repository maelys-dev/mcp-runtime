#pragma once

#include <stddef.h>
#include <jansson.h>

#include "maelys/mcp/error.h"
#include "maelys/mcp/resources.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_mcp_provider maelys_mcp_provider_t;

/*
 * The wire is negotiated, not hard-matched, for the same reason MCP's own
 * legacy revisions are (see is_supported_legacy_version): every SDK compares
 * the protocol string exactly, so a host that simply started sending a newer
 * one would break every existing provider on every request. The host opens at
 * the floor, reads the version the provider declares in its own responses,
 * and speaks that from then on. A /3 or /4 provider therefore keeps working
 * untouched; only a /4 one may send progress, and only a /5 one may open a
 * nested request back at the client.
 */
#define MAELYS_MCP_PROVIDER_PROTOCOL "maelys-provider/5"
#define MAELYS_MCP_PROVIDER_PROTOCOL_FLOOR "maelys-provider/3"
#define MAELYS_MCP_DEFAULT_PROVIDER_DESCRIBE_TIMEOUT_MS 5000u
#define MAELYS_MCP_DEFAULT_PROVIDER_CALL_TIMEOUT_MS 300000u
#define MAELYS_MCP_DEFAULT_PROVIDER_SHUTDOWN_TIMEOUT_MS 2000u

typedef enum maelys_mcp_tool_effect {
    MAELYS_MCP_EFFECT_UNSPECIFIED = 0,
    MAELYS_MCP_EFFECT_READ = 1,
    MAELYS_MCP_EFFECT_PREVIEW = 2,
    MAELYS_MCP_EFFECT_APPLY = 3,
    MAELYS_MCP_EFFECT_COMMIT = 4,
    MAELYS_MCP_EFFECT_EXECUTE = 5
} maelys_mcp_tool_effect_t;

typedef enum maelys_mcp_provider_event_kind {
    MAELYS_MCP_PROVIDER_EVENT_RESOURCE_UPDATED = 1,
    MAELYS_MCP_PROVIDER_EVENT_RESOURCES_LIST_CHANGED = 2,
    MAELYS_MCP_PROVIDER_EVENT_TOOLS_LIST_CHANGED = 3
} maelys_mcp_provider_event_kind_t;

typedef struct maelys_mcp_provider_event {
    maelys_mcp_provider_event_kind_t kind;
    const char *resource_uri;
} maelys_mcp_provider_event_t;

/* Thread-safe after the provider has been registered with a runtime. */
maelys_mcp_result_t maelys_mcp_provider_emit_event(
    maelys_mcp_provider_t *provider,
    const maelys_mcp_provider_event_t *event);

const char *maelys_mcp_tool_effect_string(maelys_mcp_tool_effect_t effect);
maelys_mcp_result_t maelys_mcp_tool_effect_parse(
    const char *value,
    maelys_mcp_tool_effect_t *out_effect);

typedef struct maelys_mcp_tool {
    const char *name;
    const char *title;
    const char *description;
    json_t *input_schema;
    json_t *output_schema;
    maelys_mcp_tool_effect_t effect;
} maelys_mcp_tool_t;

typedef enum maelys_mcp_provider_result_type {
    MAELYS_MCP_PROVIDER_RESULT_COMPLETE = 0,
    MAELYS_MCP_PROVIDER_RESULT_INPUT_REQUIRED = 1
} maelys_mcp_provider_result_type_t;

/*
 * Opaque handle for reporting progress on a long-running call. Owned by the
 * runtime and valid only for the duration of the callback it arrives in.
 */
typedef struct maelys_mcp_progress_reporter maelys_mcp_progress_reporter_t;

/*
 * Emits one notifications/progress for the call this reporter belongs to.
 * `progress` should increase across calls even when the total is unknown;
 * pass a negative `total` to omit it, and NULL `message` to omit that.
 *
 * Passing a NULL reporter is not an error: it is how "the client asked for
 * no progress" is expressed (see maelys_mcp_provider_request_t.progress), so
 * a provider can report unconditionally without testing first. Delivery is
 * best effort - progress is advisory, and a failure to enqueue it must never
 * fail the call it describes.
 */
maelys_mcp_result_t maelys_mcp_provider_report_progress(
    maelys_mcp_progress_reporter_t *reporter,
    double progress,
    double total,
    const char *message);

typedef struct maelys_mcp_provider_request {
    const char *tool_name;
    json_t *arguments;
    json_t *input_responses;
    json_t *request_state;
    json_t *client_capabilities;
    /* NULL unless the client supplied params._meta.progressToken - the spec
     * makes progress opt-in per request, so NULL means "do not bother". */
    maelys_mcp_progress_reporter_t *progress;
} maelys_mcp_provider_request_t;

typedef struct maelys_mcp_provider_result {
    maelys_mcp_provider_result_type_t type;
    json_t *content;
    json_t *structured_content;
    json_t *input_requests;
    json_t *request_state;
    int is_error;
} maelys_mcp_provider_result_t;

void maelys_mcp_provider_result_init(maelys_mcp_provider_result_t *result);
void maelys_mcp_provider_result_clear(maelys_mcp_provider_result_t *result);

typedef maelys_mcp_result_t (*maelys_mcp_provider_call_fn)(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error);

typedef maelys_mcp_result_t (*maelys_mcp_provider_read_resource_fn)(
    void *context,
    const maelys_mcp_resource_request_t *request,
    maelys_mcp_resource_result_t *out_result,
    char **out_error);

typedef void (*maelys_mcp_provider_destroy_fn)(void *context);

/*
 * Opaque handle for opening a request back at the client in the middle of a
 * call - MCP's older, nested multi-round-trip pattern, the one a
 * schema-validating `2025-11-25` client understands. Owned by the runtime and
 * valid only for the duration of the callback it arrives in.
 *
 * This is the counterpart of the resumable `input_required` result, not a
 * replacement for it: `input_required` ends the call and lets the client
 * retry, while this keeps the call open and blocks it until the client
 * answers.
 */
typedef struct maelys_mcp_nested_relay maelys_mcp_nested_relay_t;

/*
 * Sends one server-to-client request on the connection this call arrived on
 * and blocks until the client answers, the outer call is cancelled, the
 * channel goes away, or the nested deadline expires
 * (maelys_mcp_nested_config_t.request_timeout_ms, deliberately separate from
 * the provider call deadline because a human answering an elicitation
 * legitimately outlives it).
 *
 * `method` must be one of `elicitation/create`, `sampling/createMessage` or
 * `roots/list`, and the client must have declared the matching capability;
 * anything else fails with MAELYS_MCP_ERR_DENIED before a byte is sent, so a
 * provider cannot use this to reach a client surface that was never offered.
 *
 * `params` is borrowed. On MAELYS_MCP_OK `*out_result` holds the client's
 * result, owned by the caller. A client that answered with a JSON-RPC error
 * returns MAELYS_MCP_ERR_PROVIDER with that error object in `*out_result`.
 * `*out_error` is set on failure and owned by the caller.
 *
 * A NULL relay returns MAELYS_MCP_ERR_STATE: it is how "this call cannot nest"
 * is expressed, so a provider must fall back rather than assume.
 */
maelys_mcp_result_t maelys_mcp_provider_request_client(
    maelys_mcp_nested_relay_t *relay,
    const char *method,
    json_t *params,
    json_t **out_result,
    char **out_error);

/*
 * The nesting-capable forms of the two callbacks that can need a client round
 * trip. They are separate entry points rather than extra fields on
 * maelys_mcp_provider_config_t because widening a released public structure is
 * an ABI break (docs/abi-policy.md); a provider that does not register them
 * keeps using the plain callbacks unchanged.
 *
 * `relay` is NULL when the dispatch this call arrived on cannot carry a nested
 * request.
 */
typedef maelys_mcp_result_t (*maelys_mcp_provider_call_nested_fn)(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_nested_relay_t *relay,
    maelys_mcp_provider_result_t *out_result,
    char **out_error);

typedef maelys_mcp_result_t (*maelys_mcp_provider_read_resource_nested_fn)(
    void *context,
    const maelys_mcp_resource_request_t *request,
    maelys_mcp_nested_relay_t *relay,
    maelys_mcp_resource_result_t *out_result,
    char **out_error);

typedef struct maelys_mcp_provider_nested_handlers {
    maelys_mcp_provider_call_nested_fn call;
    maelys_mcp_provider_read_resource_nested_fn read_resource;
} maelys_mcp_provider_nested_handlers_t;

/*
 * Registers the nesting-capable callbacks on a provider created by
 * maelys_mcp_provider_create. Either member may be NULL, in which case that
 * surface keeps using the plain callback. Must be called before the provider
 * is handed to maelys_mcp_runtime_add_provider.
 */
maelys_mcp_result_t maelys_mcp_provider_set_nested_handlers(
    maelys_mcp_provider_t *provider,
    const maelys_mcp_provider_nested_handlers_t *handlers);

typedef struct maelys_mcp_provider_config {
    const char *name;
    const char *version;
    const maelys_mcp_tool_t *tools;
    size_t tool_count;
    const maelys_mcp_resource_t *resources;
    size_t resource_count;
    const maelys_mcp_resource_template_t *resource_templates;
    size_t resource_template_count;
    maelys_mcp_provider_call_fn call;
    maelys_mcp_provider_read_resource_fn read_resource;
    maelys_mcp_provider_destroy_fn destroy;
    void *context;
} maelys_mcp_provider_config_t;

maelys_mcp_result_t maelys_mcp_provider_create(
    const maelys_mcp_provider_config_t *config,
    maelys_mcp_provider_t **out_provider);

maelys_mcp_result_t maelys_mcp_provider_spawn(
    const char *executable_path,
    size_t max_message_bytes,
    maelys_mcp_provider_t **out_provider,
    char **out_error);

typedef struct maelys_mcp_provider_process_options {
    const char *executable_path;
    size_t max_message_bytes;
    unsigned int describe_timeout_ms;
    unsigned int call_timeout_ms;
    unsigned int shutdown_timeout_ms;
} maelys_mcp_provider_process_options_t;

maelys_mcp_result_t maelys_mcp_provider_spawn_with_options(
    const maelys_mcp_provider_process_options_t *options,
    maelys_mcp_provider_t **out_provider,
    char **out_error);

/*
 * Like maelys_mcp_provider_spawn_with_options, but carries manifest v2's
 * "args" and "executionProfile" (docs/manifest.md) through to the launch seam
 * (docs/launch-contract-design.md).
 *
 * `args` is a NULL-terminated vector of EXTRA arguments only - never the
 * complete vector. The runtime always compiles
 * argv[0] = options->executable_path, argv[1] = "--provider", then
 * args[0..], exactly as maelys_mcp_provider_spawn_with_options does with an
 * implicit empty `args`. A manifest cannot write argv[1]: see "Two layers of
 * argv" in the design document for why that invariant is native providers'
 * alone to keep. NULL (or a vector whose first element is NULL) means no
 * extra arguments and reproduces maelys_mcp_provider_spawn_with_options
 * byte-identically.
 *
 * `execution_profile` is opaque pass-through to the launcher
 * (maelys_mcp_process_spec_t.execution_profile); NULL means "the launcher's
 * own default", matching maelys_mcp_provider_spawn_with_options.
 *
 * This is a new entry point rather than new fields on the released
 * maelys_mcp_provider_process_options_t, per docs/abi-policy.md: adding
 * fields to a released public structure is an ABI break, and a compatible
 * extension is a new structure or a new entry point - the same idiom
 * maelys_mcp_provider_spawn_with_launcher already used to add a launcher
 * parameter without touching the options structure.
 */
maelys_mcp_result_t maelys_mcp_provider_spawn_with_args(
    const maelys_mcp_provider_process_options_t *options,
    char *const *args,
    const char *execution_profile,
    maelys_mcp_provider_t **out_provider,
    char **out_error);

#define MAELYS_MCP_DEFAULT_PROXY_CONNECT_TIMEOUT_MS 10000u

/*
 * What happens when an upstream tool's inputSchema fails
 * maelys_mcp_validate_schema_definition. See docs/mcp-proxy.md.
 *
 * STRICT is the zero value, deliberately, so an options struct that leaves
 * this field unset keeps the behaviour this provider shipped with: nobody
 * loses schema validation on a tool without asking for that trade-off.
 */
typedef enum maelys_mcp_proxy_schema_policy {
    /* Default: a tool this runtime cannot validate fails the whole connect,
     * naming the tool. */
    MAELYS_MCP_PROXY_SCHEMA_STRICT = 0,
    /* The tool is dropped from the exposed catalog; a call naming it fails as
     * an unknown tool, before any byte reaches the upstream. */
    MAELYS_MCP_PROXY_SCHEMA_SKIP = 1,
    /* The tool is exposed with the permissive schema {"type":"object"} (which
     * this runtime's validator accepts) and arguments are forwarded as-is;
     * the upstream is the authority on its own schema and validates on its
     * side. Effect gating, policy and audit still apply - only local
     * argument validation is delegated. */
    MAELYS_MCP_PROXY_SCHEMA_PASSTHROUGH = 2
} maelys_mcp_proxy_schema_policy_t;

/*
 * Options for federating a third-party MCP server: the runtime spawns it over
 * stdio, speaks real MCP to it as a client, and re-exposes the tools it finds
 * as an ordinary provider, so effect gating, schema validation, policy and
 * audit apply to them unchanged. See docs/mcp-proxy.md.
 */
typedef struct maelys_mcp_proxy_options {
    /* Absolute, like maelys_mcp_provider_spawn. */
    const char *executable_path;
    /* NULL-terminated argv for the upstream, or NULL for {executable_path}. */
    char *const *argv;
    size_t max_message_bytes;
    /* Covers spawn, era negotiation and the one tools/list together. */
    unsigned int connect_timeout_ms;
    unsigned int call_timeout_ms;
    /*
     * MCP has no effect classes, so this one is assigned to every upstream
     * tool. UNSPECIFIED is treated as EXECUTE, not READ: hosts gate
     * apply/commit/execute behind --allow-effect, so the conservative value a
     * caller should pass - and the one an unset field falls back to - is a
     * gated class rather than an ungated one.
     */
    maelys_mcp_tool_effect_t default_effect;
    /* Optional: "gh." exposes upstream "search" as "gh.search"; the prefix is
     * stripped again when the call is forwarded. NULL exposes names verbatim. */
    const char *tool_prefix;
    /* What to do with a tool whose inputSchema this runtime cannot validate.
     * Zero (MAELYS_MCP_PROXY_SCHEMA_STRICT) keeps today's behaviour. */
    maelys_mcp_proxy_schema_policy_t schema_policy;
} maelys_mcp_proxy_options_t;

/*
 * `out_skipped_tools` may be NULL. When schema_policy is
 * MAELYS_MCP_PROXY_SCHEMA_SKIP and at least one upstream tool was dropped
 * from the catalog for an unvalidatable schema, and out_skipped_tools is
 * non-NULL, this sets *out_skipped_tools to a caller-owned (free() it),
 * comma-separated list of the skipped tools' upstream names. Set to NULL
 * when nothing was skipped, or when the call does not return
 * MAELYS_MCP_OK.
 */
maelys_mcp_result_t maelys_mcp_provider_proxy_spawn(
    const maelys_mcp_proxy_options_t *options,
    maelys_mcp_provider_t **out_provider,
    char **out_skipped_tools,
    char **out_error);

/*
 * Like maelys_mcp_provider_proxy_spawn, but carries manifest v2's
 * "executionProfile" (docs/manifest.md) through to the launch seam. NULL
 * means "the launcher's own default", matching maelys_mcp_provider_proxy_spawn.
 * There is no proxy equivalent of maelys_mcp_provider_spawn_with_args'
 * `args`: an mcp-proxy upstream already takes its whole argv through
 * maelys_mcp_proxy_options_t.argv (see "The asymmetry with the proxy, and why
 * it is right" in docs/launch-contract-design.md).
 *
 * A new entry point rather than a new field on the released
 * maelys_mcp_proxy_options_t, for the same ABI reason as
 * maelys_mcp_provider_spawn_with_args.
 */
maelys_mcp_result_t maelys_mcp_provider_proxy_spawn_with_profile(
    const maelys_mcp_proxy_options_t *options,
    const char *execution_profile,
    maelys_mcp_provider_t **out_provider,
    char **out_skipped_tools,
    char **out_error);

void maelys_mcp_provider_destroy(maelys_mcp_provider_t *provider);

#ifdef __cplusplus
}
#endif
