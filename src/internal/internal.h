#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <pthread.h>
#include <jansson.h>

#include "maelys/mcp/content.h"
#include "maelys/mcp/channel.h"
#include "maelys/mcp/outbox.h"
#include "maelys/mcp/runtime.h"

#define MAELYS_MCP_MAX_MODULES 16u
#define JSONRPC_INVALID_REQUEST (-32600)
#define JSONRPC_METHOD_NOT_FOUND (-32601)
#define JSONRPC_INVALID_PARAMS (-32602)
#define JSONRPC_INTERNAL_ERROR (-32603)
#define MCP_SERVER_NOT_INITIALIZED (-32002)
#define MCP_POLICY_DENIED (-32003)
#define MCP_MISSING_REQUIRED_CLIENT_CAPABILITY (-32021)
#define MCP_UNSUPPORTED_VERSION (-32022)

typedef struct maelys_mcp_owned_tool {
    char *name;
    char *title;
    char *description;
    json_t *input_schema;
    json_t *output_schema;
    maelys_mcp_tool_effect_t effect;
    struct maelys_mcp_provider *provider;
} maelys_mcp_owned_tool_t;

typedef struct maelys_mcp_owned_resource {
    char *uri;
    char *name;
    char *title;
    char *description;
    char *mime_type;
    int has_size;
    long long size;
    struct maelys_mcp_provider *provider;
} maelys_mcp_owned_resource_t;

typedef struct maelys_mcp_owned_resource_template {
    char *uri_template;
    char *name;
    char *title;
    char *description;
    char *mime_type;
    struct maelys_mcp_provider *provider;
} maelys_mcp_owned_resource_template_t;

typedef struct maelys_mcp_subscription {
    json_t *id;
    char **resource_uris;
    size_t resource_uri_count;
    int tools_list_changed;
    int resources_list_changed;
    int active;
} maelys_mcp_subscription_t;

typedef enum maelys_mcp_runtime_lifecycle {
    MAELYS_MCP_RUNTIME_COLD = 0,
    MAELYS_MCP_RUNTIME_ACTIVATING = 1,
    MAELYS_MCP_RUNTIME_ACTIVE = 2,
    MAELYS_MCP_RUNTIME_FAULTED = 3,
    MAELYS_MCP_RUNTIME_SHUTTING_DOWN = 4
} maelys_mcp_runtime_lifecycle_t;

typedef enum maelys_mcp_channel_state {
    MAELYS_MCP_CHANNEL_STARTING = 0,
    MAELYS_MCP_CHANNEL_ACTIVE = 1,
    MAELYS_MCP_CHANNEL_CLOSING = 2,
    MAELYS_MCP_CHANNEL_CLOSED = 3,
    MAELYS_MCP_CHANNEL_FAULTED = 4
} maelys_mcp_channel_state_t;

/*
 * Nested runtime locks follow lifecycle_mutex -> channels_mutex ->
 * channel->mutex. provider_events_mutex is never nested with those locks.
 * A provider's event_mutex may wrap a sink callback and therefore precedes
 * provider_events_mutex, but no runtime path acquires event_mutex while holding
 * provider_events_mutex. Blocking activation, enqueue, and I/O run without the
 * lifecycle, registry, or channel metadata locks held. The channel-create
 * gate has its own mutex and condition and is never nested with another lock.
 * It admits creators before allocation, then runtime destruction closes and
 * drains it before acquiring lifecycle_mutex.
 */

struct maelys_mcp_provider {
    char *name;
    char *version;
    maelys_mcp_owned_tool_t *tools;
    size_t tool_count;
    maelys_mcp_owned_resource_t *resources;
    size_t resource_count;
    maelys_mcp_owned_resource_template_t *resource_templates;
    size_t resource_template_count;
    maelys_mcp_provider_call_fn call;
    maelys_mcp_provider_read_resource_fn read_resource;
    maelys_mcp_result_t (*activate)(void *context, char **out_error);
    int activated;
    maelys_mcp_provider_destroy_fn destroy;
    void *context;
    pthread_mutex_t event_mutex;
    int event_mutex_initialized;
    maelys_mcp_result_t (*event_sink)(
        void *context,
        const maelys_mcp_provider_event_t *event);
    void *event_sink_context;
};

struct maelys_mcp_runtime {
    char *server_name;
    char *server_version;
    char *instructions;
    size_t max_providers;
    size_t max_message_bytes;
    size_t max_subscriptions;
    maelys_mcp_provider_t **providers;
    size_t provider_count;
    const struct maelys_mcp_module_descriptor *modules[MAELYS_MCP_MAX_MODULES];
    size_t module_count;
    maelys_mcp_authorize_fn authorize;
    maelys_mcp_audit_fn audit;
    void *policy_context;
    pthread_mutex_t lifecycle_mutex;
    pthread_cond_t lifecycle_changed;
    int lifecycle_mutex_initialized;
    int lifecycle_changed_initialized;
    maelys_mcp_runtime_lifecycle_t lifecycle;
    maelys_mcp_result_t activation_status;
    int shutdown_requested;
    pthread_mutex_t channel_create_gate_mutex;
    pthread_cond_t channel_create_gate_drained;
    int channel_create_gate_mutex_initialized;
    int channel_create_gate_drained_initialized;
    int channel_create_accepting;
    size_t channel_creators_admitted;
    pthread_mutex_t channels_mutex;
    int channels_mutex_initialized;
    struct maelys_mcp_channel *channels;
    size_t live_channel_count;
    pthread_mutex_t provider_events_mutex;
    pthread_cond_t provider_events_idle;
    int provider_events_mutex_initialized;
    int provider_events_idle_initialized;
    int provider_events_accepting;
    size_t provider_events_inflight;
};

struct maelys_mcp_channel {
    maelys_mcp_runtime_t *runtime;
    struct maelys_mcp_channel *next;
    maelys_mcp_outbox_t *outbox;
    maelys_mcp_channel_config_t config;
    pthread_mutex_t mutex;
    pthread_cond_t idle;
    int mutex_initialized;
    int idle_initialized;
    maelys_mcp_channel_state_t state;
    int targetable;
    size_t operations_inflight;
    int legacy_initialize_received;
    int legacy_initialized;
    char legacy_client_name[128];
    char legacy_protocol_version[16];
    json_t *legacy_capabilities;
    maelys_mcp_subscription_t *subscriptions;
    size_t subscription_count;
};

/*
 * Transport-neutral delivery seam for one dispatched request.
 *
 * A request resolves to exactly one of:
 *   - a single buffered response (one `complete`, no `emit`) - what every
 *     request does today over stdio, and what an HTTP `application/json`
 *     reply carries;
 *   - a stream (zero or more `emit` calls carrying request-scoped
 *     notifications, then one `complete`) - what an HTTP `text/event-stream`
 *     reply carries, and what `subscriptions/listen` needs;
 *   - a stream relayed from an upstream MCP server, once this runtime can
 *     act as a gateway: the same shape, with frames originating elsewhere.
 *
 * Keeping this seam here rather than inside a transport is what makes the
 * transport itself a thin, replaceable shell.
 *
 * Ownership follows the outbox convention (see
 * maelys_mcp_outbox_enqueue_take): `emit` and `complete` steal the caller's
 * reference on success and leave it with the caller on failure.
 */
typedef struct maelys_mcp_response_sink {
    maelys_mcp_result_t (*emit)(void *context, json_t *message);
    maelys_mcp_result_t (*complete)(void *context, json_t *response);
    int (*cancelled)(void *context);
    void *context;
} maelys_mcp_response_sink_t;

/*
 * Binds a progress token to the sink of the request that carried it. Both
 * members are borrowed and outlive this struct, which the dispatching module
 * builds on its stack for exactly one provider callback.
 */
struct maelys_mcp_progress_reporter {
    const maelys_mcp_response_sink_t *sink;
    json_t *token;
};

typedef struct maelys_mcp_module_request {
    maelys_mcp_channel_t *channel;
    /* Where this request's own output goes. Borrowed from the caller, valid
     * for the duration of the dispatch. A module that produces nothing but a
     * final result never touches it - handle_with_sink completes on its
     * behalf - but a module that emits request-scoped notifications ahead of
     * that result (progress) needs it. */
    const maelys_mcp_response_sink_t *sink;
    json_t *id;
    json_t *params;
    const char *protocol_version;
    const char *client_name;
    json_t *legacy_capabilities;
    int modern;
    json_t **post_enqueue_subscription_id;
} maelys_mcp_module_request_t;

typedef struct maelys_mcp_module_descriptor {
    maelys_mcp_module_kind_t kind;
    const char *name;
    const char *capability_name;
    json_t *(*capability)(const maelys_mcp_runtime_t *runtime, int modern);
    int (*handles)(const char *method);
    json_t *(*handle)(
        maelys_mcp_runtime_t *runtime,
        const char *method,
        const maelys_mcp_module_request_t *request);
} maelys_mcp_module_descriptor_t;

typedef struct maelys_mcp_process_context {
    pid_t pid;
    int fd;
    size_t max_message_bytes;
    unsigned long long next_id;
    unsigned int describe_timeout_ms;
    unsigned int call_timeout_ms;
    unsigned int shutdown_timeout_ms;
    struct maelys_mcp_line_reader *reader;
    pthread_mutex_t exchange_mutex;
    pthread_mutex_t state_mutex;
    pthread_cond_t response_ready;
    int exchange_mutex_initialized;
    int state_mutex_initialized;
    int response_ready_initialized;
    pthread_t reader_thread;
    int reader_started;
    int closing;
    int failed;
    int events_enabled;
    int activation_pending;
    int waiting;
    maelys_mcp_result_t failure_status;
    char *failure_message;
    unsigned long long expected_id;
    json_t *pending_response;
    struct maelys_mcp_provider *owner;
} maelys_mcp_process_context_t;

typedef struct maelys_mcp_line_reader {
    char *buffer;
    size_t length;
    size_t capacity;
    size_t max_bytes;
} maelys_mcp_line_reader_t;

char *maelys_mcp_strdup(const char *value);
int maelys_mcp_monotonic_deadline(
    unsigned int timeout_ms,
    uint64_t *out_deadline_ms);
int maelys_mcp_monotonic_deadline_expired(uint64_t deadline_ms);
int maelys_mcp_cond_init_monotonic(pthread_cond_t *condition);
int maelys_mcp_cond_wait_until(
    pthread_cond_t *condition,
    pthread_mutex_t *mutex,
    uint64_t deadline_ms);

maelys_mcp_result_t maelys_mcp_stdio_finish_status(
    maelys_mcp_result_t primary_status,
    maelys_mcp_result_t close_status,
    maelys_mcp_result_t writer_status,
    maelys_mcp_result_t destroy_status,
    maelys_mcp_result_t flags_status);
int maelys_mcp_json_string_has_nul(const json_t *value);
int maelys_mcp_json_string_equals(const json_t *value, const char *expected);
int maelys_mcp_json_string_has_prefix(const json_t *value, const char *prefix);
maelys_mcp_result_t maelys_mcp_write_json_line(int fd, json_t *value);
maelys_mcp_result_t maelys_mcp_write_json_line_with_timeout(
    int fd,
    json_t *value,
    unsigned int timeout_ms);
maelys_mcp_result_t maelys_mcp_line_reader_init(
    maelys_mcp_line_reader_t *reader,
    size_t max_bytes);
void maelys_mcp_line_reader_clear(maelys_mcp_line_reader_t *reader);
maelys_mcp_result_t maelys_mcp_line_reader_next(
    maelys_mcp_line_reader_t *reader,
    json_t **out_value,
    char **out_error);
maelys_mcp_result_t maelys_mcp_line_reader_read(
    maelys_mcp_line_reader_t *reader,
    int fd,
    json_t **out_value,
    char **out_error);
maelys_mcp_result_t maelys_mcp_line_reader_read_once(
    maelys_mcp_line_reader_t *reader,
    int fd,
    json_t **out_value,
    char **out_error,
    int *out_eof);
maelys_mcp_result_t maelys_mcp_read_json_line(
    int fd,
    size_t max_bytes,
    json_t **out_value,
    char **out_error);
json_t *maelys_mcp_error_response(json_t *id, int code, const char *message, json_t *data);
json_t *maelys_mcp_success_response(json_t *id, json_t *result);
maelys_mcp_result_t maelys_mcp_validate_schema(json_t *schema, json_t *value, char **out_error);
maelys_mcp_result_t maelys_mcp_validate_schema_definition(
    json_t *schema,
    int require_object_root,
    char **out_error);
maelys_mcp_result_t maelys_mcp_isolate_stdout(int *out_transport_fd);
const maelys_mcp_module_descriptor_t *maelys_mcp_module_descriptor(
    maelys_mcp_module_kind_t kind);
json_t *maelys_mcp_runtime_capabilities(
    const maelys_mcp_runtime_t *runtime,
    int modern);
int maelys_mcp_add_server_meta(
    maelys_mcp_runtime_t *runtime,
    json_t *result,
    const char *result_type);
void maelys_mcp_subscription_clear(maelys_mcp_subscription_t *subscription);
void maelys_mcp_cancel_subscription(
    maelys_mcp_channel_t *channel,
    const json_t *request_id);
void maelys_mcp_activate_subscription(
    maelys_mcp_channel_t *channel,
    const json_t *request_id);
json_t *maelys_mcp_runtime_dispatch(
    maelys_mcp_channel_t *channel,
    json_t *request,
    const maelys_mcp_response_sink_t *sink,
    json_t **out_post_enqueue_subscription_id);
/* The sink every channel uses today: emit and complete both land in that
 * channel's outbox, which its writer thread drains. */
maelys_mcp_response_sink_t maelys_mcp_channel_outbox_sink(
    maelys_mcp_channel_t *channel);
/* maelys_mcp_channel_handle() is exactly this against the outbox sink; a
 * transport that delivers a request's response itself (HTTP) passes its own. */
maelys_mcp_result_t maelys_mcp_channel_handle_with_sink(
    maelys_mcp_channel_t *channel,
    json_t *request,
    const maelys_mcp_response_sink_t *sink);
maelys_mcp_result_t maelys_mcp_channel_enqueue_take(
    maelys_mcp_channel_t *channel,
    json_t *message,
    maelys_mcp_outbox_class_t message_class,
    const char *coalesce_key,
    int fault_on_timeout);
maelys_mcp_result_t maelys_mcp_channel_enqueue_take_until(
    maelys_mcp_channel_t *channel,
    json_t *message,
    maelys_mcp_outbox_class_t message_class,
    const char *coalesce_key,
    int fault_on_timeout,
    uint64_t deadline_ms);
maelys_mcp_result_t maelys_mcp_outbox_enqueue_take_until(
    maelys_mcp_outbox_t *outbox,
    json_t *message,
    maelys_mcp_outbox_class_t message_class,
    const char *coalesce_key,
    uint64_t deadline_ms);
maelys_mcp_result_t maelys_mcp_outbox_wait_drained_until(
    maelys_mcp_outbox_t *outbox,
    uint64_t deadline_ms);
size_t maelys_mcp_outbox_waiter_count(maelys_mcp_outbox_t *outbox);
maelys_mcp_result_t maelys_mcp_channel_complete_subscriptions_until(
    maelys_mcp_channel_t *channel,
    uint64_t deadline_ms);
void maelys_mcp_channel_abort(maelys_mcp_channel_t *channel);
maelys_mcp_result_t maelys_mcp_runtime_snapshot_channels(
    maelys_mcp_runtime_t *runtime,
    maelys_mcp_channel_t ***out_channels,
    size_t *out_count);
void maelys_mcp_channel_release_operation(maelys_mcp_channel_t *channel);
maelys_mcp_result_t maelys_mcp_runtime_begin_provider_event(
    maelys_mcp_runtime_t *runtime);
void maelys_mcp_runtime_end_provider_event(maelys_mcp_runtime_t *runtime);
void maelys_mcp_provider_bind_event_sink(
    maelys_mcp_provider_t *provider,
    maelys_mcp_result_t (*sink)(
        void *context,
        const maelys_mcp_provider_event_t *event),
    void *context);
