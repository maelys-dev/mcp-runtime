#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <pthread.h>
#include <jansson.h>

#include "maelys/mcp/content.h"
#include "maelys/mcp/channel.h"
#include "maelys/mcp/middleware.h"
#include "maelys/mcp/outbox.h"
#include "maelys/mcp/runtime.h"

#include "src/process/launcher.h"

#define MAELYS_MCP_MAX_MODULES 16u
#define MAELYS_MCP_MAX_MIDDLEWARE 16u
/*
 * Long enough for "maelys/nested/" plus a 64-bit counter and its terminator.
 * The prefix is what makes a host-generated id incapable of colliding with a
 * client-chosen one, so no id-space negotiation is needed.
 */
#define MAELYS_MCP_NESTED_ID_PREFIX "maelys/nested/"
#define MAELYS_MCP_NESTED_ID_MAX 40u
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
 *
 * A detached channel's real free runs on whichever thread released the last
 * operation reference, and takes channels_mutex to retire the channel from
 * detached_channel_count. It takes it *after* releasing channel->mutex, never
 * while holding it, so the freeing thread walks the same lifecycle_mutex ->
 * channels_mutex -> channel->mutex order downwards as everybody else rather
 * than closing a cycle back up it.
 *
 * One further edge exists, and only in one direction: a process provider's
 * state_mutex may precede a channel->mutex. It is taken when a provider
 * failure has to cancel the nested client request a worker is blocked on, so
 * that a dead provider does not strand that worker until the nested deadline.
 * Nothing acquires channel->mutex before state_mutex - the thread inside a
 * nested wait releases channel->mutex in pthread_cond_wait, and every path
 * that calls into a provider does so with no channel lock held - so the edge
 * cannot close into a cycle.
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
    /*
     * The nesting-capable forms, registered separately (see
     * maelys_mcp_provider_set_nested_handlers). When one is set the module
     * prefers it and hands it the dispatch's relay; when it is not, the plain
     * callback above runs exactly as before. Adding them here rather than to
     * maelys_mcp_provider_config_t is what kept them out of the ABI: they
     * predate ABI 4 and are not part of it.
     */
    maelys_mcp_provider_call_nested_fn call_nested;
    maelys_mcp_provider_read_resource_nested_fn read_resource_nested;
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
    /*
     * The middleware chain. Appended to under lifecycle_mutex while the
     * runtime is cold, then read without any lock for the rest of its life -
     * the immutability the public header promises is what makes that safe.
     * One counter per hook lets a dispatch path skip building a hook context
     * at all when no middleware implements that hook, so an empty chain costs
     * the same branch the removed authorize/audit pointers used to - and a
     * chain of pure authorizers costs nothing on the transformation paths.
     */
    maelys_mcp_middleware_t middleware[MAELYS_MCP_MAX_MIDDLEWARE];
    size_t middleware_count;
    size_t resolve_hook_count;
    size_t authorize_hook_count;
    size_t call_hook_count;
    size_t result_hook_count;
    size_t audit_hook_count;
    size_t wrap_sink_hook_count;
    size_t list_hook_count;
    /*
     * Nested-request settings, bound while the runtime is cold exactly like
     * the middleware chain and read without a lock afterwards. They live on
     * the runtime rather than on maelys_mcp_channel_config_t because widening
     * that released public structure would break the ABI (docs/abi-policy.md);
     * every channel inherits these at creation.
     */
    maelys_mcp_nested_config_t nested;
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
    /*
     * Channels that maelys_mcp_channel_destroy_detached has taken out of
     * `channels` but whose storage has not been released yet, because an
     * operation was still in flight when the caller gave up its handle.
     *
     * It exists because detaching *decrements* live_channel_count, so the
     * refusal at src/core/runtime.c would see zero and let the runtime be
     * freed while a detached channel still held a pointer into it. Drained by
     * maelys_mcp_runtime_destroy the way the channel-create gate is drained.
     * Guarded by channels_mutex.
     */
    size_t detached_channel_count;
    pthread_cond_t detached_channels_drained;
    int detached_channels_drained_initialized;
    pthread_mutex_t provider_events_mutex;
    pthread_cond_t provider_events_idle;
    int provider_events_mutex_initialized;
    int provider_events_idle_initialized;
    int provider_events_accepting;
    size_t provider_events_inflight;
};

/*
 * One outstanding host-to-client request, correlated by a host-generated id.
 * Sibling of maelys_mcp_subscription_t above, and guarded by the same
 * channel->mutex: the reader settles an entry, the worker blocked on it wakes
 * and takes the payload. `outer_id` is the id of the client request that
 * caused this one, so cancelling that request can reach in here.
 */
typedef struct maelys_mcp_nested_request {
    int in_use;
    int settled;
    char id[MAELYS_MCP_NESTED_ID_MAX];
    json_t *outer_id;
    maelys_mcp_result_t status;
    /* The client's `result` on success, its `error` object on a refusal. */
    json_t *payload;
} maelys_mcp_nested_request_t;

/*
 * One offloaded request. `tools/call` and `resources/read` are the only two
 * methods that can ever block on a client round trip, so they - and nothing
 * else - run on a short-lived thread, leaving the transport reader free to
 * carry that round trip's reply back. The slot outlives the thread until it is
 * reaped, which is what keeps pthread_join paired with pthread_create.
 */
typedef struct maelys_mcp_channel_worker {
    maelys_mcp_channel_t *channel;
    json_t *request;
    pthread_t thread;
    int active;
    int finished;
} maelys_mcp_channel_worker_t;

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
    /*
     * Set by maelys_mcp_channel_destroy_detached when the bounded close missed
     * its deadline: the embedder's handle is gone, the channel is already out
     * of the runtime's registry, and whichever operation reference is released
     * last performs the real free. Read and written under channel->mutex,
     * which is what makes "released last" a decidable question rather than a
     * race between the releaser and the detacher.
     */
    int detached;
    /*
     * Which protocol eras this channel serves and announces:
     * config.protocol_eras with zero normalized to MAELYS_MCP_ERA_ALL, so an
     * embedder that never mentioned eras behaves exactly as it always did.
     *
     * Written once in initialize_channel, before the channel is published and
     * therefore before any other thread can reach it, and read-only for the
     * rest of its life - which is the whole benefit of a config field over
     * the setter ABI 3 had: there is no window in which one thread narrows the
     * mask while another dispatches against it. Reads that happen to sit
     * inside a channel->mutex critical section are there for the mutable
     * fields beside them, not for this one.
     */
    unsigned int protocol_eras;
    int legacy_initialize_received;
    int legacy_initialized;
    char legacy_client_name[128];
    char legacy_protocol_version[16];
    json_t *legacy_capabilities;
    maelys_mcp_subscription_t *subscriptions;
    size_t subscription_count;
    /* Nested (in-band) host-to-client requests. See src/core/nested.c. */
    maelys_mcp_nested_request_t *nested_requests;
    size_t nested_capacity;
    unsigned long long next_nested_id;
    unsigned int nested_timeout_ms;
    pthread_cond_t nested_ready;
    int nested_ready_initialized;
    /* Offload pool for the two nestable methods. See src/core/channel.c. */
    maelys_mcp_channel_worker_t *workers;
    size_t worker_capacity;
    /*
     * Set once this channel's traffic is arriving through
     * maelys_mcp_channel_accept, which is the only entry point that offloads a
     * call and therefore the only one under which a reply to a nested request
     * can still be read. An embedder pumping maelys_mcp_channel_handle on its
     * own thread never sets it, and its provider calls are told they cannot
     * nest rather than being allowed to block on a reply nobody will read.
     */
    int transport_demuxes;
    /* First failure a worker's dispatch reported, for the transport to see. */
    maelys_mcp_result_t dispatch_status;
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
 *
 * The type name is public (maelys/mcp/middleware.h forward-declares it, so
 * hook 6 can forward to a sink through maelys_mcp_sink_emit and friends); this
 * layout is not, and stays here so that decorating the delivery path never
 * became an ABI commitment to its shape.
 */
struct maelys_mcp_response_sink {
    maelys_mcp_result_t (*emit)(void *context, json_t *message);
    maelys_mcp_result_t (*complete)(void *context, json_t *response);
    int (*cancelled)(void *context);
    void *context;
};

/*
 * Binds a progress token to the sink of the request that carried it. Both
 * members are borrowed and outlive this struct, which the dispatching module
 * builds on its stack for exactly one provider callback.
 */
struct maelys_mcp_progress_reporter {
    const maelys_mcp_response_sink_t *sink;
    json_t *token;
};

/*
 * Everything one provider callback needs to open a request back at the client
 * and block for its reply. Built on the dispatching thread's stack for exactly
 * one callback, exactly like the progress reporter above; every member except
 * the two waiter fields is borrowed from that frame.
 *
 * The nested request travels this request's own sink, not the outbox
 * directly, so it stays ordered ahead of the final response and stays visible
 * to a wrap_sink middleware rather than going around it.
 *
 * `waiter_bind` is filled in by the provider implementation, not by the
 * dispatching module: it is how a provider that can die independently (the
 * process provider) learns which nested wait to cancel when it does. NULL
 * means nothing needs telling.
 */
struct maelys_mcp_nested_relay {
    maelys_mcp_channel_t *channel;
    const maelys_mcp_response_sink_t *sink;
    json_t *outer_id;
    json_t *client_capabilities;
    void *waiter_context;
    void (*waiter_bind)(
        void *context,
        maelys_mcp_channel_t *channel,
        const char *nested_id);
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
    /* Whether this dispatch can carry a nested client request, i.e. whether
     * something other than this thread is still reading the connection. */
    int nestable;
    json_t **post_enqueue_subscription_id;
} maelys_mcp_module_request_t;

typedef struct maelys_mcp_module_descriptor {
    maelys_mcp_module_kind_t kind;
    const char *name;
    const char *capability_name;
    json_t *(*capability)(const maelys_mcp_runtime_t *runtime, int modern);
    int (*handles)(const char *method);
    /*
     * Whether this module's handling of `method` can end up blocked on a
     * client round trip, and therefore belongs on a thread of its own. The
     * predicate lives with the module for the same reason `handles` does: the
     * protocol core owns routing, not method names, and a core that had to
     * spell "tools/call" to know what to offload would have taken tools
     * knowledge back (see docs/architecture.md and scripts/audit_boundaries.sh).
     * NULL means nothing this module handles ever nests.
     */
    int (*nestable)(const char *method);
    json_t *(*handle)(
        maelys_mcp_runtime_t *runtime,
        const char *method,
        const maelys_mcp_module_request_t *request);
} maelys_mcp_module_descriptor_t;

typedef struct maelys_mcp_process_context {
    /*
     * The child, seen only through the seam: a launcher and the opaque
     * instance it handed back. No pid, deliberately - the runtime's sole
     * liveness signal is EOF on fd, which is what lets a container or an
     * executord handle stand in for a local process without a line changing
     * here (docs/launch-contract-design.md, "Provider death through the seam").
     */
    const maelys_mcp_process_launcher_t *launcher;
    maelys_mcp_process_instance_t instance;
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
    /*
     * The version this provider actually speaks. Opens at the floor and is
     * raised once the provider declares a newer one in a response, so a /3
     * provider is never sent something it would reject. Guarded by
     * state_mutex.
     */
    char negotiated_protocol[32];
    /*
     * Progress frames the reader has taken off the wire but not yet
     * delivered. The reader never emits them itself: the sink belongs to the
     * calling thread's stack frame, and its lifetime is only guaranteed while
     * that thread is inside the call. The caller drains this queue while it
     * waits, so every emission happens on the thread that owns the sink, and
     * passes through sink->emit rather than around it - which is what keeps
     * progress interceptable by a middleware sink instead of bypassing it.
     */
    json_t *pending_progress;
    /*
     * The nested request the reader has taken off the wire and not yet handed
     * to the thread inside the call, and the flag saying that thread is still
     * relaying one. Together they make "a second nested request while one is
     * outstanding" detectable in the reader, which is where the protocol says
     * it is fatal.
     */
    json_t *pending_nested;
    int nested_inflight;
    /*
     * Set only while that thread is blocked on the client. Borrowed: the
     * thread that publishes it is the thread inside the call, which holds an
     * operation reference on the channel for the whole window, so the pointer
     * cannot outlive the channel. Read under state_mutex by whichever thread
     * declares the provider failed, which then cancels the wait.
     */
    maelys_mcp_channel_t *nested_channel;
    char nested_wait_id[MAELYS_MCP_NESTED_ID_MAX];
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
/*
 * A self-pipe for waking a poll(). Both ends are close-on-exec, so no spawned
 * provider inherits them, and both are non-blocking, so neither the thread
 * raising the wakeup nor the thread lowering it can ever be parked by the
 * kernel's pipe buffer. Two callers want exactly this - the stdio transport,
 * whose reader has to learn that its writer died, and the outbox, whose
 * readiness descriptor is the same trick one layer down - and one
 * implementation is what stops the two sets of flags from drifting apart.
 * Writes -1 into both entries on failure.
 */
maelys_mcp_result_t maelys_mcp_create_wakeup_pipe(int descriptors[2]);

maelys_mcp_result_t maelys_mcp_stdio_finish_status(
    maelys_mcp_result_t primary_status,
    maelys_mcp_result_t close_status,
    maelys_mcp_result_t writer_status,
    maelys_mcp_result_t destroy_status,
    maelys_mcp_result_t flags_status);
int maelys_mcp_json_string_has_nul(const json_t *value);
int maelys_mcp_json_string_equals(const json_t *value, const char *expected);
int maelys_mcp_json_string_has_prefix(const json_t *value, const char *prefix);

/*
 * Shared input_required capability validation (mrtr.c). Returns -1 for a
 * malformed inputRequests set, 1 when a required client capability is missing
 * (collected into *out_required_capabilities for the -32021 response), 0 when
 * valid and fully declared.
 */
int maelys_mcp_validate_input_requests(
    json_t *requests,
    json_t *client_capabilities,
    json_t **out_required_capabilities,
    char **out_error);
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
/* Non-zero when an enabled module says this method can block on the client. */
int maelys_mcp_runtime_method_nestable(
    const maelys_mcp_runtime_t *runtime,
    const char *method);
int maelys_mcp_add_server_meta(
    maelys_mcp_runtime_t *runtime,
    json_t *result,
    const char *result_type);
/*
 * The middleware chain. The has_* predicates exist so a caller can skip
 * filling a hook context - list paths ask once per catalog entry - and they
 * are exactly the branch the pre-chain `if (!runtime->authorize)` was. Every
 * invocation runs with no runtime lock held.
 */
int maelys_mcp_chain_has_resolve(const maelys_mcp_runtime_t *runtime);
int maelys_mcp_chain_has_authorize(const maelys_mcp_runtime_t *runtime);
int maelys_mcp_chain_has_call(const maelys_mcp_runtime_t *runtime);
int maelys_mcp_chain_has_result(const maelys_mcp_runtime_t *runtime);
int maelys_mcp_chain_has_audit(const maelys_mcp_runtime_t *runtime);
int maelys_mcp_chain_has_wrap_sink(const maelys_mcp_runtime_t *runtime);
int maelys_mcp_chain_has_list(const maelys_mcp_runtime_t *runtime);
/*
 * Hook 1. `request` is advanced in place so each middleware sees the previous
 * one's output. On MAELYS_MCP_OK the out params hold the accumulated rewrite,
 * each NULL when nothing changed it; the caller owns both (free / json_decref).
 * On failure nothing is returned and the partial rewrite is already released.
 */
maelys_mcp_result_t maelys_mcp_chain_resolve(
    const maelys_mcp_runtime_t *runtime,
    maelys_mcp_resolve_context_t *request,
    char **out_tool_name,
    json_t **out_arguments);
maelys_mcp_authorize_decision_t maelys_mcp_chain_authorize(
    const maelys_mcp_runtime_t *runtime,
    const maelys_mcp_authorize_context_t *request);
/* Hook 3. Stops at the first substitution; out_result is filled only then. */
maelys_mcp_call_disposition_t maelys_mcp_chain_call(
    const maelys_mcp_runtime_t *runtime,
    const maelys_mcp_call_context_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error);
/*
 * Hook 4. `result` is the live result, replaced in place: the caller keeps
 * owning whatever it holds afterwards and clears it exactly once, whether the
 * chain replaced it or not.
 */
maelys_mcp_result_disposition_t maelys_mcp_chain_result(
    const maelys_mcp_runtime_t *runtime,
    maelys_mcp_result_context_t *request,
    maelys_mcp_provider_result_t *result,
    char **out_error);
void maelys_mcp_chain_audit(
    const maelys_mcp_runtime_t *runtime,
    const maelys_mcp_audit_context_t *record);
/*
 * Hook 7. Advances `request` in place like hook 1. *out_entries is NULL when
 * no middleware changed the catalog, and otherwise a new array the caller owns.
 */
maelys_mcp_result_t maelys_mcp_chain_list(
    const maelys_mcp_runtime_t *runtime,
    maelys_mcp_list_context_t *request,
    json_t **out_entries);
/*
 * Hook 6's per-request state, built on the dispatching thread's stack and
 * never shared: the sink each wrapper is exposed as, the wrapper it came
 * from, and the middleware index that owns it, so `release` can be paired
 * with the right context in reverse order.
 *
 * `guard` sits between the innermost wrapper and the real sink. It is what
 * makes "complete is forwarded exactly once" checkable rather than merely
 * asked for: it counts completions that actually reach the transport, refuses
 * a second one, and lets the caller notice a wrapper that swallowed the first.
 */
typedef struct maelys_mcp_sink_link {
    maelys_mcp_sink_wrapper_t *wrapper;
    const maelys_mcp_response_sink_t *inner;
} maelys_mcp_sink_link_t;

typedef struct maelys_mcp_sink_chain {
    size_t depth;
    maelys_mcp_response_sink_t sinks[MAELYS_MCP_MAX_MIDDLEWARE];
    maelys_mcp_sink_wrapper_t wrappers[MAELYS_MCP_MAX_MIDDLEWARE];
    maelys_mcp_sink_link_t links[MAELYS_MCP_MAX_MIDDLEWARE];
    size_t owners[MAELYS_MCP_MAX_MIDDLEWARE];
    const maelys_mcp_runtime_t *runtime;
    const maelys_mcp_response_sink_t *base;
    maelys_mcp_response_sink_t guard;
    size_t completions;
} maelys_mcp_sink_chain_t;

maelys_mcp_result_t maelys_mcp_chain_wrap_sink(
    const maelys_mcp_runtime_t *runtime,
    const maelys_mcp_wrap_sink_context_t *request,
    const maelys_mcp_response_sink_t *base,
    maelys_mcp_sink_chain_t *chain,
    const maelys_mcp_response_sink_t **out_sink);
/* Runs every wrapper's release, reverse wrapping order. Idempotent. */
void maelys_mcp_chain_release_sink(maelys_mcp_sink_chain_t *chain);
/* How many completions reached the transport. Zero after a dispatch that
 * produced a response means a wrapper swallowed it. */
size_t maelys_mcp_chain_sink_completions(const maelys_mcp_sink_chain_t *chain);
/* Runs every middleware's destroy hook, reverse registration order. */
void maelys_mcp_chain_destroy(maelys_mcp_runtime_t *runtime);
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
/*
 * One inbound transport frame, demultiplexed. This is the seam a transport
 * calls instead of maelys_mcp_channel_handle, and the reason stdio.c is a thin
 * adapter: everything that makes a connection able to carry more than one
 * conversation at a time lives here, not in the transport.
 *
 * Three outcomes, in the order they are tested:
 *   - the frame is the reply to a nested request this channel issued: it is
 *     matched against the pending table, the blocked worker is woken, and
 *     nothing is dispatched;
 *   - the frame is a `tools/call` or `resources/read` request: it is handed to
 *     a worker thread, so the caller returns to reading immediately and can
 *     carry that call's nested reply back;
 *   - anything else, a stray response included: dispatched inline, exactly as
 *     maelys_mcp_channel_handle always did.
 *
 * Borrows `message`. The returned status is the transport's, not the
 * request's: a protocol error is answered through the channel and still
 * returns MAELYS_MCP_OK.
 */
maelys_mcp_result_t maelys_mcp_channel_accept(
    maelys_mcp_channel_t *channel,
    json_t *message);
/* The first failure an offloaded dispatch reported, or MAELYS_MCP_OK. */
maelys_mcp_result_t maelys_mcp_channel_dispatch_status(
    maelys_mcp_channel_t *channel);
/*
 * The pending nested-request table. Every entry is guarded by channel->mutex
 * and woken through channel->nested_ready.
 */
maelys_mcp_result_t maelys_mcp_channel_nested_init(maelys_mcp_channel_t *channel);
void maelys_mcp_channel_nested_clear(maelys_mcp_channel_t *channel);
/* Non-zero when `message` was the reply to an outstanding nested request. */
int maelys_mcp_channel_nested_resolve(
    maelys_mcp_channel_t *channel,
    json_t *message);
/* Caller holds channel->mutex. Settles every entry, so nothing survives a
 * channel that can no longer carry a reply. */
void maelys_mcp_channel_nested_fail_all_locked(
    maelys_mcp_channel_t *channel,
    maelys_mcp_result_t status);
void maelys_mcp_channel_nested_fail_all(
    maelys_mcp_channel_t *channel,
    maelys_mcp_result_t status);
/* Settles the one entry with this host-generated id, if it is still pending. */
void maelys_mcp_channel_nested_fail_id(
    maelys_mcp_channel_t *channel,
    const char *nested_id,
    maelys_mcp_result_t status);
/* Settles every entry issued underneath the client request `outer_id`. */
void maelys_mcp_channel_nested_cancel_outer(
    maelys_mcp_channel_t *channel,
    const json_t *outer_id);
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
/*
 * The pollable outbox. maelys_mcp_outbox_next waits on a condition variable,
 * and a thread inside it is not watching a socket - so a transport whose
 * connection can go away while a provider is still running has no way to drain
 * output and notice the peer left at the same time. These two give it one
 * descriptor it can hand to poll() beside its socket.
 *
 * Internal for v1, deliberately: adding a public function later is compatible
 * and removing one is not, and nothing outside the library needs them while
 * the only consumer is in the library.
 *
 * Creates this channel's wakeup pipe so that maelys_mcp_channel_wait_fd can
 * return a descriptor. Idempotent, and called by a transport that intends to
 * poll before it dispatches anything into the channel. Lazy on purpose: stdio
 * never calls it, so stdio never allocates two descriptors per channel and
 * never pays the write(2) below - the cost lands only on the transport that
 * asked for it. Failure (descriptor exhaustion, in practice) is the caller's
 * to report; it must not be answered by falling back to short timed waits,
 * which is the polling design this seam exists to avoid.
 */
maelys_mcp_result_t maelys_mcp_channel_enable_wait_fd(
    maelys_mcp_channel_t *channel);
maelys_mcp_result_t maelys_mcp_outbox_enable_wait_fd(
    maelys_mcp_outbox_t *outbox);
/*
 * A descriptor that becomes readable whenever this channel's outbox has
 * something for maelys_mcp_channel_next - a message, or the end of a closed
 * outbox - and stays readable until it does not. Level-triggered by
 * construction: the byte is written on the transition into that state and
 * consumed on the transition out of it, so at most one byte is outstanding and
 * a burst of enqueues costs one write(2), not one per message.
 *
 * The caller must not read from it. It is for poll()/select()/kqueue only, and
 * the only correct response to it being readable is to call
 * maelys_mcp_channel_next. Spurious readability is possible and harmless -
 * next() answers ERR_TIMEOUT - but a missed wakeup is not, which is why the
 * flag is maintained under the same mutex as the queue itself. Returns -1 when
 * enable_wait_fd was never called or failed.
 */
int maelys_mcp_channel_wait_fd(const maelys_mcp_channel_t *channel);
int maelys_mcp_outbox_wait_fd(maelys_mcp_outbox_t *outbox);
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
