#include "maelys/mcp.h"
#include "src/internal/internal.h"
#include "tests/test_support.h"

#include <pthread.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static maelys_mcp_runtime_t *new_runtime(int subscriptions) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "channel-test",
        .server_version = "0.10.0",
        .max_providers = 4u,
        .max_subscriptions = 16u
    };
    maelys_mcp_runtime_t *runtime = NULL;
    if (maelys_mcp_runtime_create(&config, &runtime) != MAELYS_MCP_OK) return NULL;
    if (subscriptions &&
        (maelys_mcp_runtime_enable_module(runtime,
            MAELYS_MCP_MODULE_TOOLS) != MAELYS_MCP_OK ||
         maelys_mcp_runtime_enable_module(runtime,
            MAELYS_MCP_MODULE_RESOURCES) != MAELYS_MCP_OK ||
         maelys_mcp_runtime_enable_module(runtime,
            MAELYS_MCP_MODULE_SUBSCRIPTIONS) != MAELYS_MCP_OK)) {
        maelys_mcp_result_t destroy_status = maelys_mcp_runtime_destroy(runtime);
        (void)destroy_status;
        return NULL;
    }
    return runtime;
}

static maelys_mcp_channel_t *new_channel(
    maelys_mcp_runtime_t *runtime,
    size_t max_messages,
    unsigned int timeout_ms) {
    maelys_mcp_channel_config_t config = {
        .max_messages = max_messages,
        .max_bytes = 1024u * 1024u,
        .response_burst = 8u,
        .admission_timeout_ms = timeout_ms,
        .close_timeout_ms = 2000u
    };
    maelys_mcp_channel_t *channel = NULL;
    return maelys_mcp_channel_create(runtime, &config, &channel) == MAELYS_MCP_OK ?
        channel : NULL;
}

static json_t *modern_meta(void) {
    return json_pack("{s:s,s:{s:s,s:s},s:{}}",
        "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
        "io.modelcontextprotocol/clientInfo", "name", "channel-test", "version", "1",
        "io.modelcontextprotocol/clientCapabilities");
}

static json_t *listen_request(json_t *id, const char *uri) {
    return json_pack("{s:s,s:o,s:s,s:{s:{s:[s]},s:o}}",
        "jsonrpc", "2.0", "id", id, "method", "subscriptions/listen",
        "params", "notifications", "resourceSubscriptions", uri,
        "_meta", modern_meta());
}

static json_t *cancel_notification(json_t *id) {
    return json_pack("{s:s,s:s,s:{s:o}}",
        "jsonrpc", "2.0", "method", "notifications/cancelled",
        "params", "requestId", id);
}

static json_t *discover_request(json_int_t id) {
    return json_pack("{s:s,s:I,s:s,s:o}",
        "jsonrpc", "2.0", "id", id, "method", "server/discover",
        "params", modern_meta());
}

static maelys_mcp_result_t handle(
    maelys_mcp_channel_t *channel,
    json_t *request) {
    maelys_mcp_result_t status = maelys_mcp_channel_handle(channel, request);
    json_decref(request);
    return status;
}

static json_t *next_message(
    maelys_mcp_channel_t *channel,
    unsigned int timeout_ms) {
    json_t *message = NULL;
    return maelys_mcp_channel_next(channel, timeout_ms, &message) == MAELYS_MCP_OK ?
        message : NULL;
}

static const char *method_of(json_t *message) {
    json_t *method = json_object_get(message, "method");
    return json_is_string(method) ? json_string_value(method) : NULL;
}

static int is_resource_event(json_t *message, const char *uri) {
    const char *method = method_of(message);
    json_t *actual_uri = json_object_get(json_object_get(message, "params"), "uri");
    return method && strcmp(method, "notifications/resources/updated") == 0 &&
        json_is_string(actual_uri) && strcmp(json_string_value(actual_uri), uri) == 0;
}

static int destroy_channel_and_runtime(
    maelys_mcp_channel_t *channel,
    maelys_mcp_runtime_t *runtime) {
    return maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK &&
        maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK;
}

static int test_same_id_isolation_and_exact_fanout(void) {
    static const char *uri = "test://shared/resource";
    maelys_mcp_runtime_t *runtime = new_runtime(1);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_channel_t *channel_a = new_channel(runtime, 16u, 1000u);
    maelys_mcp_channel_t *channel_b = new_channel(runtime, 16u, 1000u);
    ASSERT_TRUE(channel_a && channel_b);
    ASSERT_TRUE(handle(channel_a, listen_request(json_integer(1), uri)) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(handle(channel_b, listen_request(json_integer(1), uri)) ==
        MAELYS_MCP_OK);
    json_t *ack_a = next_message(channel_a, 1000u);
    json_t *ack_b = next_message(channel_b, 1000u);
    ASSERT_TRUE(ack_a && ack_b);
    ASSERT_TRUE(strcmp(method_of(ack_a),
        "notifications/subscriptions/acknowledged") == 0);
    ASSERT_TRUE(strcmp(method_of(ack_b),
        "notifications/subscriptions/acknowledged") == 0);
    json_decref(ack_a);
    json_decref(ack_b);

    ASSERT_TRUE(maelys_mcp_runtime_notify_resource_updated(runtime, uri) ==
        MAELYS_MCP_OK);
    json_t *event_a = next_message(channel_a, 1000u);
    json_t *event_b = next_message(channel_b, 1000u);
    ASSERT_TRUE(event_a && event_b);
    ASSERT_TRUE(is_resource_event(event_a, uri));
    ASSERT_TRUE(is_resource_event(event_b, uri));
    json_decref(event_a);
    json_decref(event_b);
    json_t *none = NULL;
    ASSERT_TRUE(maelys_mcp_channel_next(channel_a, 20u, &none) ==
        MAELYS_MCP_ERR_TIMEOUT);
    ASSERT_TRUE(maelys_mcp_channel_next(channel_b, 20u, &none) ==
        MAELYS_MCP_ERR_TIMEOUT);

    ASSERT_TRUE(handle(channel_a, cancel_notification(json_integer(1))) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_notify_resource_updated(runtime, uri) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_channel_next(channel_a, 20u, &none) ==
        MAELYS_MCP_ERR_TIMEOUT);
    event_b = next_message(channel_b, 1000u);
    ASSERT_TRUE(event_b && is_resource_event(event_b, uri));
    json_decref(event_b);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel_a) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_ERR_STATE);
    ASSERT_TRUE(maelys_mcp_channel_complete_subscriptions(channel_b) ==
        MAELYS_MCP_OK);
    json_t *complete = next_message(channel_b, 1000u);
    ASSERT_TRUE(complete != NULL);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(
        json_object_get(complete, "result"), "resultType")), "complete") == 0);
    json_decref(complete);
    ASSERT_TRUE(destroy_channel_and_runtime(channel_b, runtime));
    return 0;
}

static int test_slow_channel_fault_is_local(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(0);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_channel_t *slow = new_channel(runtime, 1u, 20u);
    maelys_mcp_channel_t *healthy = new_channel(runtime, 8u, 1000u);
    ASSERT_TRUE(slow && healthy);
    ASSERT_TRUE(handle(slow, discover_request(1)) == MAELYS_MCP_OK);
    ASSERT_TRUE(handle(slow, discover_request(2)) == MAELYS_MCP_ERR_TIMEOUT);
    ASSERT_TRUE(handle(healthy, discover_request(3)) == MAELYS_MCP_OK);
    json_t *healthy_response = next_message(healthy, 1000u);
    ASSERT_TRUE(healthy_response != NULL);
    ASSERT_TRUE(json_integer_value(json_object_get(healthy_response, "id")) == 3);
    json_decref(healthy_response);
    json_t *first_response = next_message(slow, 1000u);
    ASSERT_TRUE(first_response != NULL);
    ASSERT_TRUE(json_integer_value(json_object_get(first_response, "id")) == 1);
    json_decref(first_response);
    ASSERT_TRUE(handle(slow, discover_request(4)) == MAELYS_MCP_ERR_STATE);
    ASSERT_TRUE(maelys_mcp_channel_destroy(slow) == MAELYS_MCP_OK);
    ASSERT_TRUE(destroy_channel_and_runtime(healthy, runtime));
    return 0;
}

static int test_slow_fanout_does_not_skip_peer(void) {
    static const char *uri = "test://slow-fanout/value";
    maelys_mcp_runtime_t *runtime = new_runtime(1);
    ASSERT_TRUE(runtime != NULL);
    /* The most recently created channel is visited first by the runtime snapshot. */
    maelys_mcp_channel_t *healthy = new_channel(runtime, 8u, 1000u);
    maelys_mcp_channel_t *slow = new_channel(runtime, 1u, 20u);
    ASSERT_TRUE(healthy && slow);
    ASSERT_TRUE(handle(healthy, listen_request(json_string("healthy"),
        "test://slow-fanout")) == MAELYS_MCP_OK);
    ASSERT_TRUE(handle(slow, listen_request(json_string("slow"),
        "test://slow-fanout")) == MAELYS_MCP_OK);
    json_t *ack = next_message(healthy, 1000u);
    ASSERT_TRUE(ack != NULL);
    json_decref(ack);
    ack = next_message(slow, 1000u);
    ASSERT_TRUE(ack != NULL);
    json_decref(ack);
    ASSERT_TRUE(handle(slow, discover_request(10)) == MAELYS_MCP_OK);

    ASSERT_TRUE(maelys_mcp_runtime_notify_resource_updated(runtime, uri) ==
        MAELYS_MCP_ERR_TIMEOUT);
    json_t *healthy_event = next_message(healthy, 1000u);
    ASSERT_TRUE(healthy_event != NULL && is_resource_event(healthy_event, uri));
    json_decref(healthy_event);
    json_t *slow_response = next_message(slow, 1000u);
    ASSERT_TRUE(slow_response != NULL);
    ASSERT_TRUE(json_integer_value(json_object_get(slow_response, "id")) == 10);
    json_decref(slow_response);

    ASSERT_TRUE(maelys_mcp_channel_complete_subscriptions(slow) == MAELYS_MCP_OK);
    json_t *complete = next_message(slow, 1000u);
    ASSERT_TRUE(complete != NULL);
    json_decref(complete);
    ASSERT_TRUE(maelys_mcp_channel_complete_subscriptions(healthy) == MAELYS_MCP_OK);
    complete = next_message(healthy, 1000u);
    ASSERT_TRUE(complete != NULL);
    json_decref(complete);
    ASSERT_TRUE(maelys_mcp_channel_destroy(slow) == MAELYS_MCP_OK);
    ASSERT_TRUE(destroy_channel_and_runtime(healthy, runtime));
    return 0;
}

typedef struct drain_context {
    maelys_mcp_channel_t *channel;
    json_t *messages[64];
    size_t count;
    maelys_mcp_result_t terminal;
} drain_context_t;

static void *drain_until_closed(void *opaque) {
    drain_context_t *context = opaque;
    for (;;) {
        json_t *message = NULL;
        maelys_mcp_result_t status = maelys_mcp_channel_next(
            context->channel, 5000u, &message);
        if (status != MAELYS_MCP_OK) {
            context->terminal = status;
            return NULL;
        }
        if (context->count < sizeof(context->messages) /
            sizeof(context->messages[0])) {
            context->messages[context->count++] = message;
        } else {
            json_decref(message);
        }
    }
}

static int test_close_drains_complete_and_wakes_consumer(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(1);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_channel_t *channel = new_channel(runtime, 8u, 1000u);
    ASSERT_TRUE(channel != NULL);
    ASSERT_TRUE(handle(channel, listen_request(json_string("close"),
        "test://close")) == MAELYS_MCP_OK);
    json_t *ack = next_message(channel, 1000u);
    ASSERT_TRUE(ack != NULL);
    json_decref(ack);
    drain_context_t context = {.channel = channel};
    pthread_t consumer;
    ASSERT_TRUE(pthread_create(&consumer, NULL, drain_until_closed, &context) == 0);
    ASSERT_TRUE(maelys_mcp_channel_close(channel, 2000u) == MAELYS_MCP_OK);
    ASSERT_TRUE(pthread_join(consumer, NULL) == 0);
    ASSERT_TRUE(context.terminal == MAELYS_MCP_ERR_CLOSED);
    ASSERT_TRUE(context.count == 1u);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(
        json_object_get(context.messages[0], "result"), "resultType")),
        "complete") == 0);
    json_decref(context.messages[0]);
    ASSERT_TRUE(maelys_mcp_channel_close(channel, 20u) == MAELYS_MCP_OK);
    ASSERT_TRUE(destroy_channel_and_runtime(channel, runtime));
    return 0;
}

typedef struct activation_context {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int entered;
    int release;
    int calls;
    maelys_mcp_result_t result;
} activation_context_t;

static void activation_context_init(
    activation_context_t *context,
    maelys_mcp_result_t result) {
    memset(context, 0, sizeof(*context));
    pthread_mutex_init(&context->mutex, NULL);
    pthread_cond_init(&context->condition, NULL);
    context->result = result;
}

static void activation_context_clear(activation_context_t *context) {
    pthread_cond_destroy(&context->condition);
    pthread_mutex_destroy(&context->mutex);
}

static maelys_mcp_result_t blocking_activate(void *opaque, char **out_error) {
    (void)out_error;
    activation_context_t *context = opaque;
    pthread_mutex_lock(&context->mutex);
    context->calls++;
    context->entered = 1;
    pthread_cond_broadcast(&context->condition);
    while (!context->release) {
        pthread_cond_wait(&context->condition, &context->mutex);
    }
    maelys_mcp_result_t result = context->result;
    pthread_mutex_unlock(&context->mutex);
    return result;
}

static void wait_for_activation(activation_context_t *context) {
    pthread_mutex_lock(&context->mutex);
    while (!context->entered) {
        pthread_cond_wait(&context->condition, &context->mutex);
    }
    pthread_mutex_unlock(&context->mutex);
}

static void release_activation(activation_context_t *context) {
    pthread_mutex_lock(&context->mutex);
    context->release = 1;
    pthread_cond_broadcast(&context->condition);
    pthread_mutex_unlock(&context->mutex);
}

static maelys_mcp_provider_t *activation_provider(
    activation_context_t *context) {
    maelys_mcp_provider_config_t config = {
        .name = "activation-provider",
        .version = "1",
        .context = context
    };
    maelys_mcp_provider_t *provider = NULL;
    if (maelys_mcp_provider_create(&config, &provider) != MAELYS_MCP_OK) return NULL;
    provider->activate = blocking_activate;
    provider->activated = 0;
    return provider;
}

typedef struct create_context {
    maelys_mcp_runtime_t *runtime;
    maelys_mcp_channel_t *channel;
    maelys_mcp_result_t status;
} create_context_t;

static void *create_channel_thread(void *opaque) {
    create_context_t *context = opaque;
    context->status = maelys_mcp_channel_create(
        context->runtime, NULL, &context->channel);
    return NULL;
}

typedef struct destroy_context {
    maelys_mcp_runtime_t *runtime;
    maelys_mcp_result_t status;
} destroy_context_t;

static void *destroy_runtime_thread(void *opaque) {
    destroy_context_t *context = opaque;
    context->status = maelys_mcp_runtime_destroy(context->runtime);
    return NULL;
}

static int wait_for_creator_count(
    maelys_mcp_runtime_t *runtime,
    size_t expected) {
    for (size_t attempt = 0; attempt < 1000u; ++attempt) {
        pthread_mutex_lock(&runtime->channel_create_gate_mutex);
        size_t count = runtime->channel_creators_admitted;
        pthread_mutex_unlock(&runtime->channel_create_gate_mutex);
        if (count == expected) return 1;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000 * 1000};
        nanosleep(&pause, NULL);
    }
    return 0;
}

static int wait_for_create_gate_closed(maelys_mcp_runtime_t *runtime) {
    for (size_t attempt = 0; attempt < 1000u; ++attempt) {
        pthread_mutex_lock(&runtime->channel_create_gate_mutex);
        int accepting = runtime->channel_create_accepting;
        pthread_mutex_unlock(&runtime->channel_create_gate_mutex);
        if (!accepting) return 1;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000 * 1000};
        nanosleep(&pause, NULL);
    }
    return 0;
}

static int test_concurrent_activation_is_unique(void) {
    activation_context_t activation;
    activation_context_init(&activation, MAELYS_MCP_OK);
    maelys_mcp_runtime_t *runtime = new_runtime(0);
    maelys_mcp_provider_t *provider = activation_provider(&activation);
    ASSERT_TRUE(runtime && provider);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, provider, NULL) ==
        MAELYS_MCP_OK);
    enum { CREATOR_COUNT = 8 };
    create_context_t creators[CREATOR_COUNT] = {0};
    pthread_t threads[CREATOR_COUNT];
    for (size_t index = 0; index < CREATOR_COUNT; ++index) {
        creators[index].runtime = runtime;
        ASSERT_TRUE(pthread_create(&threads[index], NULL,
            create_channel_thread, &creators[index]) == 0);
    }
    wait_for_activation(&activation);
    release_activation(&activation);
    for (size_t index = 0; index < CREATOR_COUNT; ++index) {
        ASSERT_TRUE(pthread_join(threads[index], NULL) == 0);
        ASSERT_TRUE(creators[index].status == MAELYS_MCP_OK);
        ASSERT_TRUE(creators[index].channel != NULL);
    }
    ASSERT_TRUE(activation.calls == 1);
    for (size_t index = 0; index < CREATOR_COUNT; ++index) {
        ASSERT_TRUE(maelys_mcp_channel_destroy(creators[index].channel) ==
            MAELYS_MCP_OK);
    }
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    activation_context_clear(&activation);
    return 0;
}

static int test_activation_failure_is_fail_closed(void) {
    activation_context_t activation;
    activation_context_init(&activation, MAELYS_MCP_ERR_PROVIDER);
    maelys_mcp_runtime_t *runtime = new_runtime(0);
    maelys_mcp_provider_t *provider = activation_provider(&activation);
    ASSERT_TRUE(runtime && provider);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, provider, NULL) ==
        MAELYS_MCP_OK);
    create_context_t creators[4] = {0};
    pthread_t threads[4];
    for (size_t index = 0; index < 4u; ++index) {
        creators[index].runtime = runtime;
        ASSERT_TRUE(pthread_create(&threads[index], NULL,
            create_channel_thread, &creators[index]) == 0);
    }
    wait_for_activation(&activation);
    release_activation(&activation);
    for (size_t index = 0; index < 4u; ++index) {
        ASSERT_TRUE(pthread_join(threads[index], NULL) == 0);
        ASSERT_TRUE(creators[index].status == MAELYS_MCP_ERR_PROVIDER);
        ASSERT_TRUE(creators[index].channel == NULL);
    }
    ASSERT_TRUE(activation.calls == 1);
    maelys_mcp_channel_t *retry = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &retry) ==
        MAELYS_MCP_ERR_PROVIDER);
    ASSERT_TRUE(retry == NULL && activation.calls == 1);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    activation_context_clear(&activation);
    return 0;
}

static int test_destroy_races_blocked_activation(void) {
    activation_context_t activation;
    activation_context_init(&activation, MAELYS_MCP_OK);
    maelys_mcp_runtime_t *runtime = new_runtime(0);
    maelys_mcp_provider_t *provider = activation_provider(&activation);
    ASSERT_TRUE(runtime && provider);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, provider, NULL) ==
        MAELYS_MCP_OK);
    enum { CREATOR_COUNT = 8 };
    create_context_t creators[CREATOR_COUNT] = {0};
    pthread_t creator_threads[CREATOR_COUNT];
    creators[0].runtime = runtime;
    ASSERT_TRUE(pthread_create(&creator_threads[0], NULL,
        create_channel_thread, &creators[0]) == 0);
    wait_for_activation(&activation);
    for (size_t index = 1; index < CREATOR_COUNT; ++index) {
        creators[index].runtime = runtime;
        ASSERT_TRUE(pthread_create(&creator_threads[index], NULL,
            create_channel_thread, &creators[index]) == 0);
    }
    ASSERT_TRUE(wait_for_creator_count(runtime, CREATOR_COUNT));
    destroy_context_t destroyer = {.runtime = runtime};
    pthread_t destroyer_thread;
    ASSERT_TRUE(pthread_create(&destroyer_thread, NULL,
        destroy_runtime_thread, &destroyer) == 0);
    release_activation(&activation);
    for (size_t index = 0; index < CREATOR_COUNT; ++index) {
        ASSERT_TRUE(pthread_join(creator_threads[index], NULL) == 0);
        ASSERT_TRUE(creators[index].status == MAELYS_MCP_OK);
        ASSERT_TRUE(creators[index].channel != NULL);
    }
    ASSERT_TRUE(pthread_join(destroyer_thread, NULL) == 0);
    ASSERT_TRUE(destroyer.status == MAELYS_MCP_ERR_STATE);
    for (size_t index = 0; index < CREATOR_COUNT; ++index) {
        ASSERT_TRUE(maelys_mcp_channel_destroy(creators[index].channel) ==
            MAELYS_MCP_OK);
    }
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    ASSERT_TRUE(activation.calls == 1);
    activation_context_clear(&activation);
    return 0;
}

static int test_create_after_gate_is_hardening_not_contract(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(0);
    ASSERT_TRUE(runtime != NULL);
    enum { CREATOR_COUNT = 8 };
    create_context_t creators[CREATOR_COUNT] = {0};
    pthread_t creator_threads[CREATOR_COUNT];
    pthread_mutex_lock(&runtime->lifecycle_mutex);
    for (size_t index = 0; index < CREATOR_COUNT; ++index) {
        creators[index].runtime = runtime;
        ASSERT_TRUE(pthread_create(&creator_threads[index], NULL,
            create_channel_thread, &creators[index]) == 0);
    }
    ASSERT_TRUE(wait_for_creator_count(runtime, CREATOR_COUNT));
    destroy_context_t destroyer = {.runtime = runtime};
    pthread_t destroyer_thread;
    ASSERT_TRUE(pthread_create(&destroyer_thread, NULL,
        destroy_runtime_thread, &destroyer) == 0);
    ASSERT_TRUE(wait_for_create_gate_closed(runtime));
    /*
     * This call starts after runtime_destroy and is outside the public
     * lifetime contract. The earlier admitted creators keep the object live,
     * so it exercises only the defensive closed-gate behavior.
     */
    maelys_mcp_channel_t *late_channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &late_channel) ==
        MAELYS_MCP_ERR_STATE);
    ASSERT_TRUE(late_channel == NULL);
    pthread_mutex_unlock(&runtime->lifecycle_mutex);
    for (size_t index = 0; index < CREATOR_COUNT; ++index) {
        ASSERT_TRUE(pthread_join(creator_threads[index], NULL) == 0);
        ASSERT_TRUE(creators[index].status == MAELYS_MCP_OK);
        ASSERT_TRUE(creators[index].channel != NULL);
    }
    ASSERT_TRUE(pthread_join(destroyer_thread, NULL) == 0);
    ASSERT_TRUE(destroyer.status == MAELYS_MCP_ERR_STATE);
    for (size_t index = 0; index < CREATOR_COUNT; ++index) {
        ASSERT_TRUE(maelys_mcp_channel_destroy(creators[index].channel) ==
            MAELYS_MCP_OK);
    }
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

typedef struct provider_callback_context {
    maelys_mcp_runtime_t *runtime;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    size_t admitted;
    int release;
} provider_callback_context_t;

static maelys_mcp_result_t hold_provider_event_callback(
    void *opaque,
    const maelys_mcp_provider_event_t *event) {
    provider_callback_context_t *context = opaque;
    (void)event;
    if (maelys_mcp_runtime_begin_provider_event(context->runtime) !=
        MAELYS_MCP_OK) {
        return MAELYS_MCP_ERR_NOT_FOUND;
    }
    pthread_mutex_lock(&context->mutex);
    context->admitted++;
    pthread_cond_broadcast(&context->changed);
    while (!context->release) {
        pthread_cond_wait(&context->changed, &context->mutex);
    }
    pthread_mutex_unlock(&context->mutex);
    maelys_mcp_runtime_end_provider_event(context->runtime);
    return MAELYS_MCP_OK;
}

typedef struct provider_emit_context {
    maelys_mcp_provider_t *provider;
    maelys_mcp_result_t status;
} provider_emit_context_t;

static void *emit_provider_event(void *opaque) {
    provider_emit_context_t *context = opaque;
    const maelys_mcp_provider_event_t event = {
        .kind = MAELYS_MCP_PROVIDER_EVENT_TOOLS_LIST_CHANGED
    };
    context->status = maelys_mcp_provider_emit_event(context->provider, &event);
    return NULL;
}

typedef struct barrier_destroy_context {
    maelys_mcp_runtime_t *runtime;
    maelys_mcp_result_t status;
    atomic_int done;
} barrier_destroy_context_t;

static void *destroy_runtime_with_completion(void *opaque) {
    barrier_destroy_context_t *context = opaque;
    context->status = maelys_mcp_runtime_destroy(context->runtime);
    atomic_store(&context->done, 1);
    return NULL;
}

static int wait_for_provider_event_shutdown(maelys_mcp_runtime_t *runtime) {
    for (size_t attempt = 0; attempt < 1000u; ++attempt) {
        pthread_mutex_lock(&runtime->provider_events_mutex);
        int accepting = runtime->provider_events_accepting;
        pthread_mutex_unlock(&runtime->provider_events_mutex);
        if (!accepting) return 1;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000 * 1000};
        nanosleep(&pause, NULL);
    }
    return 0;
}

static int test_runtime_destroy_waits_for_multi_provider_callbacks(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(0);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_provider_t *providers[2] = {NULL, NULL};
    const maelys_mcp_provider_config_t provider_configs[2] = {
        {.name = "barrier-provider-a", .version = "1"},
        {.name = "barrier-provider-b", .version = "1"}
    };
    provider_callback_context_t callback_barrier = {.runtime = runtime};
    ASSERT_TRUE(pthread_mutex_init(&callback_barrier.mutex, NULL) == 0);
    ASSERT_TRUE(pthread_cond_init(&callback_barrier.changed, NULL) == 0);
    provider_emit_context_t emits[2] = {0};
    pthread_t callback_threads[2];
    for (size_t index = 0; index < 2u; ++index) {
        ASSERT_TRUE(maelys_mcp_provider_create(&provider_configs[index],
            &providers[index]) == MAELYS_MCP_OK);
        ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, providers[index],
            NULL) == MAELYS_MCP_OK);
        maelys_mcp_provider_bind_event_sink(providers[index],
            hold_provider_event_callback, &callback_barrier);
        emits[index].provider = providers[index];
    }
    maelys_mcp_channel_t *activation_channel = new_channel(runtime, 8u, 100u);
    ASSERT_TRUE(activation_channel != NULL);
    ASSERT_TRUE(maelys_mcp_channel_destroy(activation_channel) == MAELYS_MCP_OK);
    for (size_t index = 0; index < 2u; ++index) {
        ASSERT_TRUE(pthread_create(&callback_threads[index], NULL,
            emit_provider_event, &emits[index]) == 0);
    }
    pthread_mutex_lock(&callback_barrier.mutex);
    while (callback_barrier.admitted != 2u) {
        pthread_cond_wait(&callback_barrier.changed, &callback_barrier.mutex);
    }
    pthread_mutex_unlock(&callback_barrier.mutex);

    barrier_destroy_context_t destroyer = {.runtime = runtime};
    atomic_init(&destroyer.done, 0);
    pthread_t destroyer_thread;
    ASSERT_TRUE(pthread_create(&destroyer_thread, NULL,
        destroy_runtime_with_completion, &destroyer) == 0);
    ASSERT_TRUE(wait_for_provider_event_shutdown(runtime));
    ASSERT_TRUE(atomic_load(&destroyer.done) == 0);

    pthread_mutex_lock(&callback_barrier.mutex);
    callback_barrier.release = 1;
    pthread_cond_broadcast(&callback_barrier.changed);
    pthread_mutex_unlock(&callback_barrier.mutex);
    for (size_t index = 0; index < 2u; ++index) {
        ASSERT_TRUE(pthread_join(callback_threads[index], NULL) == 0);
        ASSERT_TRUE(emits[index].status == MAELYS_MCP_OK);
    }
    ASSERT_TRUE(pthread_join(destroyer_thread, NULL) == 0);
    ASSERT_TRUE(destroyer.status == MAELYS_MCP_OK);
    pthread_cond_destroy(&callback_barrier.changed);
    pthread_mutex_destroy(&callback_barrier.mutex);
    return 0;
}

static uint64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0u;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static int test_close_uses_one_absolute_deadline(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(1);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_channel_t *channel = new_channel(runtime, 4u, 100u);
    ASSERT_TRUE(channel != NULL);
    for (json_int_t id = 1; id <= 4; ++id) {
        ASSERT_TRUE(handle(channel, listen_request(
            json_integer(id), "test://deadline")) == MAELYS_MCP_OK);
    }
    uint64_t started = monotonic_ms();
    ASSERT_TRUE(started != 0u);
    ASSERT_TRUE(maelys_mcp_channel_close(channel, 40u) ==
        MAELYS_MCP_ERR_TIMEOUT);
    uint64_t elapsed = monotonic_ms() - started;
    ASSERT_TRUE(elapsed < 200u);
    maelys_mcp_channel_abort(channel);
    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

static int test_destroy_consumes_channel_after_bounded_close_failure(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(1);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_channel_config_t config = {
        .max_messages = 4u,
        .max_bytes = 1024u * 1024u,
        .response_burst = 8u,
        .admission_timeout_ms = 100u,
        .close_timeout_ms = 10u
    };
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, &config, &channel) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(handle(channel, listen_request(json_string("terminal-destroy"),
        "test://terminal")) == MAELYS_MCP_OK);
    json_t *ack = next_message(channel, 1000u);
    ASSERT_TRUE(ack != NULL);
    json_decref(ack);

    /* The close queues complete but no consumer drains it before the deadline. */
    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_ERR_TIMEOUT);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

typedef struct event_context {
    maelys_mcp_provider_t *provider;
    atomic_int delivered;
    atomic_int failed;
} event_context_t;

static void *emit_many_events(void *opaque) {
    event_context_t *context = opaque;
    maelys_mcp_provider_event_t event = {
        .kind = MAELYS_MCP_PROVIDER_EVENT_RESOURCE_UPDATED,
        .resource_uri = "test://fanout/value"
    };
    for (size_t index = 0; index < 500u; ++index) {
        maelys_mcp_result_t status = maelys_mcp_provider_emit_event(
            context->provider, &event);
        if (status == MAELYS_MCP_OK) atomic_fetch_add(&context->delivered, 1);
        else if (status != MAELYS_MCP_ERR_CLOSED &&
                 status != MAELYS_MCP_ERR_TIMEOUT) {
            atomic_store(&context->failed, 1);
            break;
        }
    }
    return NULL;
}

typedef struct one_event_context {
    maelys_mcp_provider_t *provider;
    const char *uri;
    maelys_mcp_result_t status;
} one_event_context_t;

static void *emit_one_resource_event(void *opaque) {
    one_event_context_t *context = opaque;
    const maelys_mcp_provider_event_t event = {
        .kind = MAELYS_MCP_PROVIDER_EVENT_RESOURCE_UPDATED,
        .resource_uri = context->uri
    };
    context->status = maelys_mcp_provider_emit_event(context->provider, &event);
    return NULL;
}

typedef struct channel_destroy_context {
    maelys_mcp_channel_t *channel;
    maelys_mcp_result_t status;
} channel_destroy_context_t;

static void *destroy_channel_thread(void *opaque) {
    channel_destroy_context_t *context = opaque;
    context->status = maelys_mcp_channel_destroy(context->channel);
    return NULL;
}

static int wait_for_channel_state(
    maelys_mcp_channel_t *channel,
    maelys_mcp_channel_state_t state) {
    for (size_t attempt = 0; attempt < 1000u; ++attempt) {
        pthread_mutex_lock(&channel->mutex);
        int matches = channel->state == state;
        pthread_mutex_unlock(&channel->mutex);
        if (matches) return 1;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000 * 1000};
        nanosleep(&pause, NULL);
    }
    return 0;
}

static int wait_for_channel_operation(
    maelys_mcp_channel_t *channel) {
    for (size_t attempt = 0; attempt < 1000u; ++attempt) {
        pthread_mutex_lock(&channel->mutex);
        int held = channel->operations_inflight != 0u;
        pthread_mutex_unlock(&channel->mutex);
        if (held) return 1;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000 * 1000};
        nanosleep(&pause, NULL);
    }
    return 0;
}

static int test_destroy_during_fanout_keeps_other_channel_live(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(1);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_provider_config_t provider_config = {
        .name = "destroy-event-provider", .version = "1"
    };
    maelys_mcp_provider_t *provider = NULL;
    ASSERT_TRUE(maelys_mcp_provider_create(&provider_config, &provider) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, provider, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel_a = new_channel(runtime, 1u, 1000u);
    maelys_mcp_channel_t *channel_b = new_channel(runtime, 8u, 1000u);
    ASSERT_TRUE(channel_a && channel_b);
    ASSERT_TRUE(handle(channel_a, listen_request(json_string("destroy-A"),
        "test://fanout/")) == MAELYS_MCP_OK);
    ASSERT_TRUE(handle(channel_b, listen_request(json_string("destroy-B"),
        "test://fanout/")) == MAELYS_MCP_OK);
    json_t *message = next_message(channel_a, 1000u);
    ASSERT_TRUE(message != NULL);
    json_decref(message);
    message = next_message(channel_b, 1000u);
    ASSERT_TRUE(message != NULL);
    json_decref(message);

    const maelys_mcp_provider_event_t first_event = {
        .kind = MAELYS_MCP_PROVIDER_EVENT_RESOURCE_UPDATED,
        .resource_uri = "test://fanout/first"
    };
    ASSERT_TRUE(maelys_mcp_provider_emit_event(provider, &first_event) ==
        MAELYS_MCP_OK);

    one_event_context_t producer = {
        .provider = provider,
        .uri = "test://fanout/second"
    };
    pthread_t producer_thread;
    ASSERT_TRUE(pthread_create(&producer_thread, NULL,
        emit_one_resource_event, &producer) == 0);
    ASSERT_TRUE(wait_for_channel_operation(channel_a));

    channel_destroy_context_t destroyer = {.channel = channel_a};
    pthread_t destroyer_thread;
    ASSERT_TRUE(pthread_create(&destroyer_thread, NULL,
        destroy_channel_thread, &destroyer) == 0);
    ASSERT_TRUE(wait_for_channel_state(channel_a, MAELYS_MCP_CHANNEL_CLOSING));

    message = next_message(channel_a, 1000u);
    ASSERT_TRUE(message && is_resource_event(message, "test://fanout/first"));
    json_decref(message);
    ASSERT_TRUE(pthread_join(producer_thread, NULL) == 0);
    ASSERT_TRUE(producer.status == MAELYS_MCP_OK);

    message = next_message(channel_a, 1000u);
    ASSERT_TRUE(message && is_resource_event(message, "test://fanout/second"));
    json_decref(message);
    message = next_message(channel_a, 1000u);
    ASSERT_TRUE(message != NULL);
    json_decref(message);
    ASSERT_TRUE(pthread_join(destroyer_thread, NULL) == 0);
    ASSERT_TRUE(destroyer.status == MAELYS_MCP_OK);

    for (size_t index = 0; index < 2u; ++index) {
        message = next_message(channel_b, 1000u);
        ASSERT_TRUE(message && is_resource_event(message,
            index == 0u ? "test://fanout/first" : "test://fanout/second"));
        json_decref(message);
    }
    ASSERT_TRUE(maelys_mcp_channel_complete_subscriptions(channel_b) ==
        MAELYS_MCP_OK);
    message = next_message(channel_b, 1000u);
    ASSERT_TRUE(message != NULL);
    json_decref(message);
    ASSERT_TRUE(destroy_channel_and_runtime(channel_b, runtime));
    return 0;
}

static int test_close_during_fanout_keeps_other_channel_live(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(1);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_provider_config_t provider_config = {
        .name = "event-provider", .version = "1"
    };
    maelys_mcp_provider_t *provider = NULL;
    ASSERT_TRUE(maelys_mcp_provider_create(&provider_config, &provider) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, provider, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel_a = new_channel(runtime, 8u, 100u);
    maelys_mcp_channel_t *channel_b = new_channel(runtime, 8u, 100u);
    ASSERT_TRUE(channel_a && channel_b);
    ASSERT_TRUE(handle(channel_a, listen_request(json_string("A"),
        "test://fanout")) == MAELYS_MCP_OK);
    ASSERT_TRUE(handle(channel_b, listen_request(json_string("B"),
        "test://fanout")) == MAELYS_MCP_OK);
    json_t *ack = next_message(channel_a, 1000u);
    ASSERT_TRUE(ack != NULL);
    json_decref(ack);
    ack = next_message(channel_b, 1000u);
    ASSERT_TRUE(ack != NULL);
    json_decref(ack);

    drain_context_t drain_a = {.channel = channel_a};
    pthread_t drain_thread;
    ASSERT_TRUE(pthread_create(&drain_thread, NULL,
        drain_until_closed, &drain_a) == 0);
    event_context_t events = {.provider = provider};
    atomic_init(&events.delivered, 0);
    atomic_init(&events.failed, 0);
    pthread_t event_thread;
    ASSERT_TRUE(pthread_create(&event_thread, NULL, emit_many_events, &events) == 0);
    ASSERT_TRUE(maelys_mcp_channel_close(channel_a, 2000u) == MAELYS_MCP_OK);
    ASSERT_TRUE(pthread_join(drain_thread, NULL) == 0);
    ASSERT_TRUE(pthread_join(event_thread, NULL) == 0);
    ASSERT_TRUE(drain_a.terminal == MAELYS_MCP_ERR_CLOSED);
    ASSERT_TRUE(atomic_load(&events.failed) == 0);
    ASSERT_TRUE(atomic_load(&events.delivered) > 0);
    for (size_t index = 0; index < drain_a.count; ++index) {
        json_decref(drain_a.messages[index]);
    }
    json_t *event_b = next_message(channel_b, 1000u);
    ASSERT_TRUE(event_b != NULL);
    ASSERT_TRUE(is_resource_event(event_b, "test://fanout/value"));
    json_decref(event_b);
    ASSERT_TRUE(maelys_mcp_channel_destroy(channel_a) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_channel_complete_subscriptions(channel_b) ==
        MAELYS_MCP_OK);
    for (;;) {
        json_t *message = NULL;
        maelys_mcp_result_t status = maelys_mcp_channel_next(
            channel_b, 20u, &message);
        if (status == MAELYS_MCP_ERR_TIMEOUT) break;
        ASSERT_TRUE(status == MAELYS_MCP_OK && message != NULL);
        json_decref(message);
    }
    ASSERT_TRUE(destroy_channel_and_runtime(channel_b, runtime));
    return 0;
}

static int test_zero_channel_event_and_argument_contracts(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(1);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_provider_config_t provider_config = {
        .name = "zero-channel-provider", .version = "1"
    };
    maelys_mcp_provider_t *provider = NULL;
    ASSERT_TRUE(maelys_mcp_provider_create(&provider_config, &provider) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, provider, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = new_channel(runtime, 8u, 100u);
    ASSERT_TRUE(channel != NULL);
    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    maelys_mcp_provider_event_t event = {
        .kind = MAELYS_MCP_PROVIDER_EVENT_TOOLS_LIST_CHANGED
    };
    ASSERT_TRUE(maelys_mcp_provider_emit_event(provider, &event) ==
        MAELYS_MCP_ERR_NOT_FOUND);
    ASSERT_TRUE(maelys_mcp_channel_create(NULL, NULL, &channel) ==
        MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);
    json_t *invalid_request = json_object();
    ASSERT_TRUE(invalid_request != NULL);
    ASSERT_TRUE(maelys_mcp_channel_handle(NULL, invalid_request) ==
        MAELYS_MCP_ERR_ARGUMENT);
    json_decref(invalid_request);
    ASSERT_TRUE(maelys_mcp_channel_next(NULL, 1u, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_channel_close(NULL, 1u) == MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_channel_destroy(NULL) == MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(NULL) == MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * The steady state an HTTP transport produces, which no test above covers.
 *
 * Each pairwise interaction is already exercised: fanout versus close,
 * fanout versus destroy, concurrent activation, concurrent creation. What
 * has never run is the combination - many channels dispatching against one
 * runtime and one in-process provider, while fanout targets them and other
 * channels are created and destroyed underneath. That is exactly what a
 * channel-per-connection HTTP transport does on every request, and the
 * in-process provider call path has no lock of its own (unlike an
 * out-of-process one, serialised by exchange_mutex), so concurrent entry
 * into a provider's call function is itself newly exercised here.
 *
 * Backpressure and empty-registry outcomes are tolerated deliberately: with
 * a bounded outbox, a fanout thread running flat out and channels churning,
 * a close legitimately times out and a fanout legitimately finds no live
 * channel. Asserting otherwise would make this flaky. The strict assertions
 * are that no operation returns a status outside those, and that real work
 * happened; race detection is TSan's job, which is why this belongs in
 * `make tsan` as much as in `make check`.
 */
typedef struct churn_context {
    maelys_mcp_runtime_t *runtime;
    int iterations;
    atomic_int dispatched;
    atomic_int unexpected;
} churn_context_t;

typedef struct churn_event_context {
    maelys_mcp_provider_t *provider;
    atomic_int delivered;
    atomic_int unexpected;
    atomic_int stop;
} churn_event_context_t;

static maelys_mcp_result_t churn_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    (void)context; (void)request; (void)out_error;
    out_result->type = MAELYS_MCP_PROVIDER_RESULT_COMPLETE;
    out_result->structured_content = json_pack("{s:b}", "ok", 1);
    return out_result->structured_content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static json_t *churn_call_request(json_int_t id) {
    return json_pack("{s:s,s:I,s:s,s:{s:s,s:{},s:o}}",
        "jsonrpc", "2.0", "id", id, "method", "tools/call",
        "params", "name", "churn.noop", "arguments", "_meta", modern_meta());
}

/* Only these can arise once a bounded outbox meets a saturating fanout. */
static int churn_tolerated(maelys_mcp_result_t status) {
    return status == MAELYS_MCP_OK || status == MAELYS_MCP_ERR_CLOSED ||
        status == MAELYS_MCP_ERR_TIMEOUT || status == MAELYS_MCP_ERR_STATE;
}

/* Unlike emit_many_events, this tolerates NOT_FOUND: the workers destroy and
 * recreate channels constantly, so there are instants with no live channel at
 * all, which snapshot_channels reports as NOT_FOUND. That is the fanout
 * equivalent of an HTTP server between requests, not a failure. It runs until
 * told to stop, so it spans exactly the workers' lifetime. */
static void *churn_events(void *opaque) {
    churn_event_context_t *context = opaque;
    maelys_mcp_provider_event_t event = {
        .kind = MAELYS_MCP_PROVIDER_EVENT_RESOURCE_UPDATED,
        .resource_uri = "test://fanout/value"
    };
    while (!atomic_load(&context->stop)) {
        maelys_mcp_result_t status = maelys_mcp_provider_emit_event(
            context->provider, &event);
        if (status == MAELYS_MCP_OK) {
            atomic_fetch_add(&context->delivered, 1);
        } else if (status != MAELYS_MCP_ERR_NOT_FOUND &&
                   status != MAELYS_MCP_ERR_CLOSED &&
                   status != MAELYS_MCP_ERR_TIMEOUT) {
            atomic_fetch_add(&context->unexpected, 1);
            return NULL;
        }
    }
    return NULL;
}

/* new_channel hardcodes a two-second close timeout. This test deliberately
 * abandons an outbox the fanout thread keeps filling, so a close that does
 * time out has to be cheap rather than dominate the run. */
static maelys_mcp_channel_t *churn_channel(maelys_mcp_runtime_t *runtime) {
    maelys_mcp_channel_config_t config = {
        .max_messages = 32u,
        .max_bytes = 1024u * 1024u,
        .response_burst = 8u,
        .admission_timeout_ms = 100u,
        .close_timeout_ms = 100u
    };
    maelys_mcp_channel_t *channel = NULL;
    return maelys_mcp_channel_create(runtime, &config, &channel) == MAELYS_MCP_OK ?
        channel : NULL;
}

static void *churn_worker(void *opaque) {
    churn_context_t *context = opaque;
    for (int index = 0; index < context->iterations; ++index) {
        maelys_mcp_channel_t *channel = churn_channel(context->runtime);
        if (!channel) {
            atomic_fetch_add(&context->unexpected, 1);
            return NULL;
        }
        /* Subscribe, so fanout genuinely targets this channel while it is
         * dispatching rather than skipping it. */
        maelys_mcp_result_t status = handle(channel,
            listen_request(json_string("churn"), "test://fanout"));
        if (!churn_tolerated(status)) atomic_fetch_add(&context->unexpected, 1);

        status = handle(channel, churn_call_request((json_int_t)index + 1));
        if (!churn_tolerated(status)) atomic_fetch_add(&context->unexpected, 1);
        if (status == MAELYS_MCP_OK) atomic_fetch_add(&context->dispatched, 1);

        /* Drain the listen ack, the call response and whatever fanout landed,
         * so close usually has nothing left to wait for. Bounded, because the
         * fanout thread keeps producing for as long as it runs. */
        for (int drained = 0; drained < 64; ++drained) {
            json_t *message = next_message(channel, 20u);
            if (!message) break;
            json_decref(message);
        }
        /* A timeout is the documented outcome when the outbox still holds
         * messages no consumer will take: channel_destroy aborts and consumes
         * ownership regardless. Against a saturating fanout that is a
         * legitimate race, not a defect. */
        maelys_mcp_result_t destroyed = maelys_mcp_channel_destroy(channel);
        if (destroyed != MAELYS_MCP_OK && destroyed != MAELYS_MCP_ERR_TIMEOUT) {
            atomic_fetch_add(&context->unexpected, 1);
        }
    }
    return NULL;
}

static int test_concurrent_channels_dispatch_under_fanout(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(1);
    ASSERT_TRUE(runtime != NULL);

    maelys_mcp_tool_t tool = {
        .name = "churn.noop",
        .title = "No-op",
        .description = "Returns a constant so the dispatch path reaches a provider.",
        .effect = MAELYS_MCP_EFFECT_READ
    };
    tool.input_schema = json_pack("{s:s}", "type", "object");
    ASSERT_TRUE(tool.input_schema != NULL);
    maelys_mcp_provider_config_t provider_config = {
        .name = "churn-provider", .version = "1",
        .tools = &tool, .tool_count = 1,
        .call = churn_call
    };
    maelys_mcp_provider_t *provider = NULL;
    maelys_mcp_result_t created = maelys_mcp_provider_create(&provider_config, &provider);
    json_decref(tool.input_schema);
    ASSERT_TRUE(created == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, provider, NULL) == MAELYS_MCP_OK);

    enum { WORKERS = 6, ITERATIONS = 25 };
    churn_context_t context = {.runtime = runtime, .iterations = ITERATIONS};
    atomic_init(&context.dispatched, 0);
    atomic_init(&context.unexpected, 0);
    churn_event_context_t events = {.provider = provider};
    atomic_init(&events.delivered, 0);
    atomic_init(&events.unexpected, 0);
    atomic_init(&events.stop, 0);

    pthread_t workers[WORKERS];
    pthread_t event_thread;
    ASSERT_TRUE(pthread_create(&event_thread, NULL, churn_events, &events) == 0);
    for (size_t index = 0; index < WORKERS; ++index) {
        ASSERT_TRUE(pthread_create(&workers[index], NULL, churn_worker, &context) == 0);
    }
    for (size_t index = 0; index < WORKERS; ++index) {
        ASSERT_TRUE(pthread_join(workers[index], NULL) == 0);
    }
    atomic_store(&events.stop, 1);
    ASSERT_TRUE(pthread_join(event_thread, NULL) == 0);

    ASSERT_TRUE(atomic_load(&context.unexpected) == 0);
    ASSERT_TRUE(atomic_load(&events.unexpected) == 0);
    /* Guards against a vacuous pass: if every channel had faulted at once, or
     * fanout had never reached a live channel, nothing would be proven. */
    ASSERT_TRUE(atomic_load(&context.dispatched) > (WORKERS * ITERATIONS) / 2);
    ASSERT_TRUE(atomic_load(&events.delivered) > 0);

    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

static int test_channel_context_accessor(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(0);
    ASSERT_TRUE(runtime != NULL);

    maelys_mcp_channel_t *bare_channel = new_channel(runtime, 8u, 1000u);
    ASSERT_TRUE(bare_channel != NULL);
    ASSERT_TRUE(maelys_mcp_channel_context(bare_channel) == NULL);
    ASSERT_TRUE(maelys_mcp_channel_destroy(bare_channel) == MAELYS_MCP_OK);

    int sentinel = 0;
    maelys_mcp_channel_config_t config = {
        .max_messages = 8u,
        .max_bytes = 1024u * 1024u,
        .response_burst = 8u,
        .admission_timeout_ms = 1000u,
        .close_timeout_ms = 1000u,
        .context = &sentinel
    };
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, &config, &channel) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_channel_context(channel) == &sentinel);

    ASSERT_TRUE(handle(channel, discover_request(1)) == MAELYS_MCP_OK);
    json_t *response = next_message(channel, 1000u);
    ASSERT_TRUE(response != NULL);
    json_decref(response);

    /* Same pointer, read again after a dispatch, for the channel's lifetime. */
    ASSERT_TRUE(maelys_mcp_channel_context(channel) == &sentinel);
    ASSERT_TRUE(maelys_mcp_channel_context(NULL) == NULL);

    ASSERT_TRUE(destroy_channel_and_runtime(channel, runtime));
    return 0;
}

/* ---- tools/list schemas across channels, concurrently ---- */

static json_t *listing_input_schema(void) {
    return json_pack("{s:s,s:{s:{s:s}},s:[s],s:b}",
        "type", "object",
        "properties", "path", "type", "string",
        "required", "path",
        "additionalProperties", 0);
}

static json_t *listing_output_schema(void) {
    return json_pack("{s:s,s:{s:{s:s}}}",
        "type", "object",
        "properties", "text", "type", "string");
}

/* Never reached: this suite only lists the tool. */
static maelys_mcp_result_t listing_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    (void)context;
    (void)request;
    (void)out_result;
    (void)out_error;
    return MAELYS_MCP_ERR_STATE;
}

static maelys_mcp_runtime_t *new_listing_runtime(void) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "listing-test",
        .server_version = "1",
        .max_providers = 1u
    };
    maelys_mcp_runtime_t *runtime = NULL;
    if (maelys_mcp_runtime_create(&config, &runtime) != MAELYS_MCP_OK) return NULL;
    json_t *input_schema = listing_input_schema();
    json_t *output_schema = listing_output_schema();
    maelys_mcp_tool_t tool = {
        .name = "ls.read",
        .title = "Read",
        .description = "Lists with both schemas declared.",
        .input_schema = input_schema,
        .output_schema = output_schema,
        .effect = MAELYS_MCP_EFFECT_READ
    };
    maelys_mcp_provider_config_t provider_config = {
        .name = "listing-provider",
        .version = "1",
        .tools = &tool,
        .tool_count = 1u,
        .call = listing_call
    };
    maelys_mcp_provider_t *provider = NULL;
    maelys_mcp_result_t status = input_schema && output_schema &&
        maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_TOOLS) ==
            MAELYS_MCP_OK ?
        maelys_mcp_provider_create(&provider_config, &provider) :
        MAELYS_MCP_ERR_STATE;
    if (input_schema) json_decref(input_schema);
    if (output_schema) json_decref(output_schema);
    if (status == MAELYS_MCP_OK &&
        maelys_mcp_runtime_add_provider(runtime, provider, NULL) != MAELYS_MCP_OK) {
        maelys_mcp_provider_destroy(provider);
        status = MAELYS_MCP_ERR_STATE;
    }
    if (status != MAELYS_MCP_OK) {
        maelys_mcp_result_t destroy_status = maelys_mcp_runtime_destroy(runtime);
        (void)destroy_status;
        return NULL;
    }
    return runtime;
}

static json_t *list_request(json_int_t id) {
    return json_pack("{s:s,s:I,s:s,s:{s:o}}",
        "jsonrpc", "2.0", "id", id, "method", "tools/list",
        "params", "_meta", modern_meta());
}

#define LISTING_ROUNDS 32

typedef struct lister {
    maelys_mcp_channel_t *channel;
    int failures;
} lister_t;

/*
 * One channel's worth of the stdio writer's life: take the response off the
 * outbox, serialize it, release it. The serialize step matters - it is where
 * a frame still sharing nodes with the registry would be read while another
 * thread manipulates those nodes' refcounts.
 */
static void *list_repeatedly(void *argument) {
    lister_t *lister = argument;
    for (int round = 0; round < LISTING_ROUNDS; ++round) {
        if (handle(lister->channel, list_request(round + 1)) != MAELYS_MCP_OK) {
            lister->failures++;
            continue;
        }
        json_t *response = next_message(lister->channel, 5000u);
        json_t *tools = json_object_get(
            json_object_get(response, "result"), "tools");
        char *wire = response ?
            json_dumps(response, JSON_COMPACT | JSON_SORT_KEYS) : NULL;
        if (!response || json_array_size(tools) != 1u || !wire) {
            lister->failures++;
        }
        free(wire);
        if (response) json_decref(response);
    }
    return NULL;
}

/*
 * A tools/list response must share no node with the registry's tool entries:
 * the registry co-owns its schema objects for the life of the runtime, and a
 * response referencing them is released by whichever thread drains that
 * channel's outbox while other channels' dispatches take references to the
 * same nodes. Nothing orders those threads, and jansson's refcount is an
 * atomic read-modify-write behind an ordinary read of the same word, so the
 * share is a data race - the same letter-of-the-memory-model race the stdio
 * transport's shared id and progressToken nodes were. Two channels listing
 * concurrently is the smallest shape that exercises it.
 */
static int test_tools_list_shares_no_schema_across_channels(void) {
    maelys_mcp_runtime_t *runtime = new_listing_runtime();
    ASSERT_TRUE(runtime != NULL);
    lister_t first = {0};
    lister_t second = {0};
    first.channel = new_channel(runtime, 16u, 5000u);
    second.channel = new_channel(runtime, 16u, 5000u);
    ASSERT_TRUE(first.channel && second.channel);

    pthread_t thread;
    ASSERT_TRUE(pthread_create(&thread, NULL, list_repeatedly, &second) == 0);
    list_repeatedly(&first);
    ASSERT_TRUE(pthread_join(thread, NULL) == 0);
    ASSERT_TRUE(first.failures == 0 && second.failures == 0);

    /* Not sharing must not mean not equal: the wire still carries exactly
     * the schemas the provider declared. */
    ASSERT_TRUE(handle(first.channel, list_request(LISTING_ROUNDS + 1)) ==
        MAELYS_MCP_OK);
    json_t *response = next_message(first.channel, 5000u);
    ASSERT_TRUE(response != NULL);
    json_t *entry = json_array_get(
        json_object_get(json_object_get(response, "result"), "tools"), 0u);
    json_t *expected_input = listing_input_schema();
    json_t *expected_output = listing_output_schema();
    int matches = expected_input && expected_output &&
        json_equal(json_object_get(entry, "inputSchema"), expected_input) &&
        json_equal(json_object_get(entry, "outputSchema"), expected_output);
    if (expected_input) json_decref(expected_input);
    if (expected_output) json_decref(expected_output);
    json_decref(response);
    ASSERT_TRUE(matches);

    ASSERT_TRUE(maelys_mcp_channel_destroy(second.channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(destroy_channel_and_runtime(first.channel, runtime));
    return 0;
}

/*
 * Detachable destruction. The shape every test below needs is one in-process
 * provider call that will not return until the test says so, which is exactly
 * the failure maelys_mcp_channel_destroy_detached exists for: a bounded close
 * that cannot succeed, on a channel nobody can free yet.
 */
/*
 * Watches one maelys_mcp_channel_config_t::context_release: how often it ran,
 * with what, on which thread, and - the two properties that are the whole
 * point of the callback - whether it ran while a worker was still inside the
 * provider, and whether it ran late enough that maelys_mcp_runtime_destroy had
 * already returned. Every field is atomic because the callback is entitled to
 * run on a thread the test never created.
 */
typedef struct release_probe {
    atomic_int calls;
    atomic_int wrong_arguments;
    atomic_int saw_a_live_provider_call;
    atomic_int ran_after_runtime_destroy;
    atomic_int ran_off_the_creating_thread;
    /* Raised by the stuck provider for as long as one of its calls is inside
     * the dispatch this channel is carrying. */
    atomic_int provider_inside;
    atomic_int runtime_destroy_returned;
    pthread_t creating_thread;
    void *expected_context;
    void *expected_release_context;
} release_probe_t;

static void release_probe_init(release_probe_t *probe, void *context) {
    memset(probe, 0, sizeof(*probe));
    atomic_store(&probe->calls, 0);
    atomic_store(&probe->wrong_arguments, 0);
    atomic_store(&probe->saw_a_live_provider_call, 0);
    atomic_store(&probe->ran_after_runtime_destroy, 0);
    atomic_store(&probe->ran_off_the_creating_thread, 0);
    atomic_store(&probe->provider_inside, 0);
    atomic_store(&probe->runtime_destroy_returned, 0);
    probe->creating_thread = pthread_self();
    probe->expected_context = context;
    probe->expected_release_context = probe;
}

static void release_probe_callback(void *release_context, void *context) {
    release_probe_t *probe = release_context;
    if (!probe) return;
    if (context != probe->expected_context ||
        release_context != probe->expected_release_context) {
        atomic_fetch_add(&probe->wrong_arguments, 1);
    }
    if (atomic_load(&probe->provider_inside) != 0) {
        atomic_fetch_add(&probe->saw_a_live_provider_call, 1);
    }
    if (atomic_load(&probe->runtime_destroy_returned) != 0) {
        atomic_fetch_add(&probe->ran_after_runtime_destroy, 1);
    }
    if (!pthread_equal(pthread_self(), probe->creating_thread)) {
        atomic_fetch_add(&probe->ran_off_the_creating_thread, 1);
    }
    atomic_fetch_add(&probe->calls, 1);
}

static int release_probe_is_clean(const release_probe_t *probe) {
    return atomic_load(&probe->wrong_arguments) == 0 &&
        atomic_load(&probe->saw_a_live_provider_call) == 0 &&
        atomic_load(&probe->ran_after_runtime_destroy) == 0;
}

typedef struct stuck_provider {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    /* Counts, not flags. A test that parks two calls has to wait for the
     * second one to arrive rather than for evidence that the first did, and
     * the mixed case needs to let exactly one of them go. Each call takes a
     * ticket on entry and leaves once release_through has reached it. */
    int entered;
    int release_through;
    /* Optional, and only the release tests set it: the probe that wants to
     * know whether a context was handed back while this call was still
     * running. */
    release_probe_t *probe;
} stuck_provider_t;

static maelys_mcp_result_t stuck_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    stuck_provider_t *stuck = context;
    (void)request;
    (void)out_error;
    if (stuck->probe) atomic_fetch_add(&stuck->probe->provider_inside, 1);
    pthread_mutex_lock(&stuck->mutex);
    int ticket = ++stuck->entered;
    pthread_cond_broadcast(&stuck->changed);
    while (stuck->release_through < ticket) {
        pthread_cond_wait(&stuck->changed, &stuck->mutex);
    }
    pthread_mutex_unlock(&stuck->mutex);
    out_result->type = MAELYS_MCP_PROVIDER_RESULT_COMPLETE;
    out_result->structured_content = json_pack("{s:b}", "ok", 1);
    if (stuck->probe) atomic_fetch_sub(&stuck->probe->provider_inside, 1);
    return out_result->structured_content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static void stuck_provider_init(stuck_provider_t *stuck) {
    pthread_mutex_init(&stuck->mutex, NULL);
    pthread_cond_init(&stuck->changed, NULL);
    stuck->entered = 0;
    stuck->release_through = 0;
    stuck->probe = NULL;
}

static void stuck_provider_clear(stuck_provider_t *stuck) {
    pthread_cond_destroy(&stuck->changed);
    pthread_mutex_destroy(&stuck->mutex);
}

static void stuck_provider_await_entries(stuck_provider_t *stuck, int count) {
    pthread_mutex_lock(&stuck->mutex);
    while (stuck->entered < count) {
        pthread_cond_wait(&stuck->changed, &stuck->mutex);
    }
    pthread_mutex_unlock(&stuck->mutex);
}

static void stuck_provider_release_through(
    stuck_provider_t *stuck,
    int ticket) {
    pthread_mutex_lock(&stuck->mutex);
    stuck->release_through = ticket;
    pthread_cond_broadcast(&stuck->changed);
    pthread_mutex_unlock(&stuck->mutex);
}

static void stuck_provider_release(stuck_provider_t *stuck) {
    stuck_provider_release_through(stuck, INT_MAX);
}

/* Spins until the channel reports `expected` operation references, so a test
 * can tell "the worker has finished" from "the worker is about to". Returns
 * non-zero on success. */
static int await_operations(maelys_mcp_channel_t *channel, size_t expected) {
    for (int attempt = 0; attempt < 5000; ++attempt) {
        pthread_mutex_lock(&channel->mutex);
        size_t inflight = channel->operations_inflight;
        pthread_mutex_unlock(&channel->mutex);
        if (inflight == expected) return 1;
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000 * 1000 };
        nanosleep(&pause, NULL);
    }
    return 0;
}

static maelys_mcp_runtime_t *new_stuck_runtime(
    stuck_provider_t *stuck,
    maelys_mcp_provider_t **out_provider) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "detach-test",
        .server_version = "1",
        .max_providers = 2u,
        .max_subscriptions = 8u
    };
    maelys_mcp_runtime_t *runtime = NULL;
    if (maelys_mcp_runtime_create(&config, &runtime) != MAELYS_MCP_OK) return NULL;
    if (maelys_mcp_runtime_enable_module(runtime,
            MAELYS_MCP_MODULE_TOOLS) != MAELYS_MCP_OK) {
        return NULL;
    }
    maelys_mcp_tool_t tool = {
        .name = "detach.stuck",
        .title = "Stuck",
        .description = "Blocks inside the provider until the test releases it.",
        .effect = MAELYS_MCP_EFFECT_READ
    };
    tool.input_schema = json_pack("{s:s}", "type", "object");
    if (!tool.input_schema) return NULL;
    maelys_mcp_provider_config_t provider_config = {
        .name = "stuck-provider", .version = "1",
        .tools = &tool, .tool_count = 1,
        .call = stuck_call, .context = stuck
    };
    maelys_mcp_provider_t *provider = NULL;
    maelys_mcp_result_t created = maelys_mcp_provider_create(&provider_config,
        &provider);
    json_decref(tool.input_schema);
    if (created != MAELYS_MCP_OK ||
        maelys_mcp_runtime_add_provider(runtime, provider, NULL) != MAELYS_MCP_OK) {
        return NULL;
    }
    if (out_provider) *out_provider = provider;
    return runtime;
}

static json_t *stuck_call_request(json_int_t id) {
    return json_pack("{s:s,s:I,s:s,s:{s:s,s:{},s:o}}",
        "jsonrpc", "2.0", "id", id, "method", "tools/call",
        "params", "name", "detach.stuck", "arguments", "_meta", modern_meta());
}

static maelys_mcp_channel_t *new_detach_channel(maelys_mcp_runtime_t *runtime) {
    maelys_mcp_channel_config_t config = {
        .max_messages = 8u,
        .max_bytes = 1024u * 1024u,
        .response_burst = 8u,
        .admission_timeout_ms = 200u,
        .close_timeout_ms = 50u
    };
    maelys_mcp_channel_t *channel = NULL;
    return maelys_mcp_channel_create(runtime, &config, &channel) == MAELYS_MCP_OK ?
        channel : NULL;
}

/*
 * The core promise: the caller is not held hostage by the provider. The call
 * is offloaded through maelys_mcp_channel_accept, so the operation reference
 * that outlives the destroy is held by a worker thread - the case that cannot
 * be fixed by joining, because the freeing thread would be joining itself.
 */
static int test_detached_destroy_returns_while_a_provider_is_stuck(void) {
    stuck_provider_t stuck;
    stuck_provider_init(&stuck);
    maelys_mcp_runtime_t *runtime = new_stuck_runtime(&stuck, NULL);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_channel_t *channel = new_detach_channel(runtime);
    ASSERT_TRUE(channel != NULL);
    json_t *call = stuck_call_request(1);
    ASSERT_TRUE(call != NULL);
    ASSERT_TRUE(maelys_mcp_channel_accept(channel, call) == MAELYS_MCP_OK);
    json_decref(call);
    stuck_provider_await_entries(&stuck, 1);

    uint64_t started = monotonic_ms();
    ASSERT_TRUE(started != 0u);
    ASSERT_TRUE(maelys_mcp_channel_destroy_detached(channel) ==
        MAELYS_MCP_ERR_TIMEOUT);
    uint64_t elapsed = monotonic_ms() - started;
    /*
     * Generous against a loaded CI box and still an order of magnitude below
     * "as long as the provider takes", which is the status quo this replaces
     * and which here is unbounded by construction.
     */
    ASSERT_TRUE(elapsed < 1000u);

    stuck_provider_release(&stuck);
    /* The real free happens on the worker; runtime_destroy waits for it. */
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    stuck_provider_clear(&stuck);
    return 0;
}

/*
 * destroy_context_t's sibling, with the one thing the drain tests need that it
 * does not have: a flag the test can read *before* joining. The assertion these
 * tests make is that runtime_destroy has NOT returned yet, and a join cannot
 * express that.
 */
typedef struct watched_destroy_context {
    maelys_mcp_runtime_t *runtime;
    maelys_mcp_result_t status;
    atomic_int returned;
} watched_destroy_context_t;

static void *watched_destroy_runtime_thread(void *opaque) {
    watched_destroy_context_t *context = opaque;
    context->status = maelys_mcp_runtime_destroy(context->runtime);
    atomic_store(&context->returned, 1);
    return NULL;
}

/*
 * The hazard the design priced and the reason detached_channel_count exists:
 * detaching *decrements* live_channel_count, so a runtime_destroy that only
 * consulted that counter would free the runtime out from under a channel that
 * still points at it. This asserts the negative - that destroy has NOT
 * returned while the provider is stuck - which is the only form in which the
 * bug is visible before it becomes a use-after-free.
 */
static int test_runtime_destroy_waits_for_a_detached_channel(void) {
    stuck_provider_t stuck;
    stuck_provider_init(&stuck);
    maelys_mcp_runtime_t *runtime = new_stuck_runtime(&stuck, NULL);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_channel_t *channel = new_detach_channel(runtime);
    ASSERT_TRUE(channel != NULL);
    json_t *call = stuck_call_request(1);
    ASSERT_TRUE(call != NULL);
    ASSERT_TRUE(maelys_mcp_channel_accept(channel, call) == MAELYS_MCP_OK);
    json_decref(call);
    stuck_provider_await_entries(&stuck, 1);
    ASSERT_TRUE(maelys_mcp_channel_destroy_detached(channel) ==
        MAELYS_MCP_ERR_TIMEOUT);

    watched_destroy_context_t context = { .runtime = runtime,
        .status = MAELYS_MCP_OK };
    atomic_store(&context.returned, 0);
    pthread_t thread;
    ASSERT_TRUE(pthread_create(&thread, NULL, watched_destroy_runtime_thread,
        &context) == 0);
    for (int slept = 0; slept < 20; ++slept) {
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
        nanosleep(&pause, NULL);
    }
    ASSERT_TRUE(atomic_load(&context.returned) == 0);

    stuck_provider_release(&stuck);
    ASSERT_TRUE(pthread_join(thread, NULL) == 0);
    ASSERT_TRUE(context.status == MAELYS_MCP_OK);
    stuck_provider_clear(&stuck);
    return 0;
}

typedef struct inline_call_context {
    maelys_mcp_channel_t *channel;
    maelys_mcp_result_t status;
} inline_call_context_t;

static void *handle_stuck_call(void *opaque) {
    inline_call_context_t *context = opaque;
    json_t *call = stuck_call_request(1);
    if (!call) {
        context->status = MAELYS_MCP_ERR_MEMORY;
        return NULL;
    }
    context->status = maelys_mcp_channel_handle(context->channel, call);
    json_decref(call);
    return NULL;
}

/*
 * The other release path. maelys_mcp_channel_handle takes its operation
 * reference on the caller's thread and gives it back through
 * maelys_mcp_channel_release_operation, which is a different line of code from
 * the worker's own decrement; both have to be able to perform the final free,
 * and a fix applied to only one of them passes the test above.
 */
static int test_detached_destroy_frees_from_an_inline_operation(void) {
    stuck_provider_t stuck;
    stuck_provider_init(&stuck);
    maelys_mcp_runtime_t *runtime = new_stuck_runtime(&stuck, NULL);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_channel_t *channel = new_detach_channel(runtime);
    ASSERT_TRUE(channel != NULL);
    inline_call_context_t context = { .channel = channel,
        .status = MAELYS_MCP_OK };
    pthread_t thread;
    ASSERT_TRUE(pthread_create(&thread, NULL, handle_stuck_call, &context) == 0);
    stuck_provider_await_entries(&stuck, 1);

    uint64_t started = monotonic_ms();
    ASSERT_TRUE(maelys_mcp_channel_destroy_detached(channel) ==
        MAELYS_MCP_ERR_TIMEOUT);
    ASSERT_TRUE(monotonic_ms() - started < 1000u);
    stuck_provider_release(&stuck);
    ASSERT_TRUE(pthread_join(thread, NULL) == 0);
    /* The aborted channel refused this request's output; that is the abort
     * working, not a defect. */
    ASSERT_TRUE(context.status != MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    stuck_provider_clear(&stuck);
    return 0;
}

/*
 * Nothing in flight is the common case, and it must not become the detached
 * case: with no operation left to perform the free, a channel handed off to
 * nobody would never be released at all. Two shapes - a clean close, and a
 * close that times out on an undrained outbox with no operation outstanding.
 */
static int test_detached_destroy_frees_inline_when_nothing_is_in_flight(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(1);
    ASSERT_TRUE(runtime != NULL);
    ASSERT_TRUE(maelys_mcp_channel_destroy_detached(NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);

    maelys_mcp_channel_t *clean = new_channel(runtime, 4u, 100u);
    ASSERT_TRUE(clean != NULL);
    ASSERT_TRUE(maelys_mcp_channel_destroy_detached(clean) == MAELYS_MCP_OK);
    /* Freed inline, so the runtime is already destroyable. */
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);

    runtime = new_runtime(1);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_channel_config_t config = {
        .max_messages = 4u,
        .max_bytes = 1024u * 1024u,
        .response_burst = 8u,
        .admission_timeout_ms = 100u,
        .close_timeout_ms = 10u
    };
    maelys_mcp_channel_t *stalled = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, &config, &stalled) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(handle(stalled, listen_request(json_string("detach-stall"),
        "test://terminal")) == MAELYS_MCP_OK);
    json_t *ack = next_message(stalled, 1000u);
    ASSERT_TRUE(ack != NULL);
    json_decref(ack);
    /* The close queues complete and no consumer drains it before the deadline,
     * so close fails - but no operation is outstanding, so the detaching
     * thread is itself the last reference and frees before returning. */
    ASSERT_TRUE(maelys_mcp_channel_destroy_detached(stalled) ==
        MAELYS_MCP_ERR_TIMEOUT);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * A channel the embedder already closed itself, then handed to the detaching
 * destroy: the second call must be an ordinary free, not a second detach.
 */
static int test_detached_destroy_after_an_explicit_close(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(1);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_channel_t *channel = new_channel(runtime, 4u, 100u);
    ASSERT_TRUE(channel != NULL);
    ASSERT_TRUE(maelys_mcp_channel_close(channel, 1000u) == MAELYS_MCP_OK);
    /* close is idempotent, and the destroy that follows it still frees. */
    ASSERT_TRUE(maelys_mcp_channel_close(channel, 1000u) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_channel_destroy_detached(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * Two channels detached at once, both stuck, released together: the drain has
 * to be a count rather than a flag, and the last free has to be the one that
 * wakes runtime_destroy.
 */
static int test_two_detached_channels_both_drain(void) {
    stuck_provider_t stuck;
    stuck_provider_init(&stuck);
    maelys_mcp_runtime_t *runtime = new_stuck_runtime(&stuck, NULL);
    ASSERT_TRUE(runtime != NULL);
    for (int index = 0; index < 2; ++index) {
        maelys_mcp_channel_t *channel = new_detach_channel(runtime);
        ASSERT_TRUE(channel != NULL);
        json_t *call = stuck_call_request(index + 1);
        ASSERT_TRUE(call != NULL);
        ASSERT_TRUE(maelys_mcp_channel_accept(channel, call) == MAELYS_MCP_OK);
        json_decref(call);
        stuck_provider_await_entries(&stuck, index + 1);
        ASSERT_TRUE(maelys_mcp_channel_destroy_detached(channel) ==
            MAELYS_MCP_ERR_TIMEOUT);
    }
    watched_destroy_context_t context = { .runtime = runtime,
        .status = MAELYS_MCP_OK };
    atomic_store(&context.returned, 0);
    pthread_t thread;
    ASSERT_TRUE(pthread_create(&thread, NULL, watched_destroy_runtime_thread,
        &context) == 0);
    for (int slept = 0; slept < 10; ++slept) {
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
        nanosleep(&pause, NULL);
    }
    ASSERT_TRUE(atomic_load(&context.returned) == 0);
    stuck_provider_release(&stuck);
    ASSERT_TRUE(pthread_join(thread, NULL) == 0);
    ASSERT_TRUE(context.status == MAELYS_MCP_OK);
    stuck_provider_clear(&stuck);
    return 0;
}

/*
 * The mixed case, and the only shape in which both halves of the worker
 * hand-off run at once: one worker has already finished when the detach
 * happens and one is still inside the provider. The finished slot is the
 * detacher's to join; the running one disposes of its own handle at its tail.
 * Getting the exclusivity wrong either leaves a thread nobody disposes of or
 * disposes of one twice, and neither is reachable with a single worker.
 *
 * The close has to fail on the *operations* drain rather than on the outbox,
 * because maelys_mcp_channel_close reaps finished workers itself once the
 * drain succeeds - so a channel that closed cleanly never reaches the detach
 * with a finished slot still in the array.
 */
static int test_detached_destroy_joins_a_finished_worker(void) {
    stuck_provider_t stuck;
    stuck_provider_init(&stuck);
    maelys_mcp_runtime_t *runtime = new_stuck_runtime(&stuck, NULL);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_channel_t *channel = new_detach_channel(runtime);
    ASSERT_TRUE(channel != NULL);
    for (int index = 0; index < 2; ++index) {
        json_t *call = stuck_call_request(index + 1);
        ASSERT_TRUE(call != NULL);
        ASSERT_TRUE(maelys_mcp_channel_accept(channel, call) == MAELYS_MCP_OK);
        json_decref(call);
    }
    stuck_provider_await_entries(&stuck, 2);
    ASSERT_TRUE(await_operations(channel, 2u));
    /* Let exactly the first one out. Its slot is now active-and-finished, and
     * nothing has reaped it: reap_workers runs from accept and from a close
     * that drained, and neither has happened. */
    stuck_provider_release_through(&stuck, 1);
    ASSERT_TRUE(await_operations(channel, 1u));

    uint64_t started = monotonic_ms();
    ASSERT_TRUE(maelys_mcp_channel_destroy_detached(channel) ==
        MAELYS_MCP_ERR_TIMEOUT);
    ASSERT_TRUE(monotonic_ms() - started < 1000u);

    stuck_provider_release(&stuck);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    stuck_provider_clear(&stuck);
    return 0;
}

/*
 * context_release, the ABI 4 half of detachable destruction. The callback
 * exists so that an embedder whose channel context owns something - an
 * authenticated principal, in the case it was designed for - learns when that
 * something stops being reachable, instead of having to wait for
 * maelys_mcp_runtime_destroy. "Exactly once, whichever path destruction took"
 * is the entire contract, so both paths are asserted, and the count is
 * asserted to be zero at the points where a wrong implementation would already
 * have called it.
 */
static maelys_mcp_channel_config_t release_config(
    release_probe_t *probe,
    void *context,
    unsigned int close_timeout_ms) {
    maelys_mcp_channel_config_t config = {
        .max_messages = 8u,
        .max_bytes = 1024u * 1024u,
        .response_burst = 8u,
        .admission_timeout_ms = 200u,
        .close_timeout_ms = close_timeout_ms,
        .context = context,
        .context_release = release_probe_callback,
        .release_context = probe
    };
    return config;
}

static int test_context_release_runs_once_on_the_synchronous_destroy(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(0);
    ASSERT_TRUE(runtime != NULL);
    int principal = 0;
    release_probe_t probe;
    release_probe_init(&probe, &principal);
    maelys_mcp_channel_config_t config = release_config(&probe, &principal,
        2000u);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, &config, &channel) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_channel_context(channel) == &principal);

    /* A channel that is being used is not a channel that is being released. */
    ASSERT_TRUE(handle(channel, discover_request(1)) == MAELYS_MCP_OK);
    json_t *response = next_message(channel, 1000u);
    ASSERT_TRUE(response != NULL);
    json_decref(response);
    ASSERT_TRUE(atomic_load(&probe.calls) == 0);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(atomic_load(&probe.calls) == 1);
    ASSERT_TRUE(release_probe_is_clean(&probe));
    /* The synchronous path frees on the caller's own thread, so this one is
     * not merely "some thread": it is this one. */
    ASSERT_TRUE(atomic_load(&probe.ran_off_the_creating_thread) == 0);

    /*
     * The detaching destroy on a channel that closes cleanly frees inline
     * through the very same function, and must therefore also release exactly
     * once - the case a fix applied only to the deferred path would miss.
     */
    int second_principal = 0;
    release_probe_t clean_probe;
    release_probe_init(&clean_probe, &second_principal);
    maelys_mcp_channel_config_t clean_config = release_config(&clean_probe,
        &second_principal, 2000u);
    maelys_mcp_channel_t *clean = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, &clean_config, &clean) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_channel_destroy_detached(clean) == MAELYS_MCP_OK);
    ASSERT_TRUE(atomic_load(&clean_probe.calls) == 1);
    ASSERT_TRUE(release_probe_is_clean(&clean_probe));

    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    /* Still one. A second call from the runtime teardown would be a double
     * free in an embedder that took the contract at its word. */
    ASSERT_TRUE(atomic_load(&probe.calls) == 1);
    ASSERT_TRUE(atomic_load(&clean_probe.calls) == 1);
    return 0;
}

/*
 * Ownership transfers on success and only on success. A create that refuses
 * its config never took the context, and a create that allocated a channel and
 * then failed to publish it must not hand back a context its caller still
 * owns - it would be releasing something on behalf of a channel that never
 * existed.
 */
static int test_context_release_is_not_called_when_creation_fails(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(0);
    ASSERT_TRUE(runtime != NULL);
    int principal = 0;
    release_probe_t probe;
    release_probe_init(&probe, &principal);
    maelys_mcp_channel_config_t config = release_config(&probe, &principal,
        1000u);
    config.protocol_eras = MAELYS_MCP_ERA_ALL | 1u << 7;
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, &config, &channel) ==
        MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(channel == NULL && atomic_load(&probe.calls) == 0);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    ASSERT_TRUE(atomic_load(&probe.calls) == 0);

    /* The other failure shape: the config was copied into a real channel, and
     * then provider activation refused to let it be published. */
    activation_context_t activation;
    activation_context_init(&activation, MAELYS_MCP_ERR_PROVIDER);
    runtime = new_runtime(0);
    maelys_mcp_provider_t *provider = activation_provider(&activation);
    ASSERT_TRUE(runtime && provider);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, provider, NULL) ==
        MAELYS_MCP_OK);
    release_probe_t unpublished;
    release_probe_init(&unpublished, &principal);
    maelys_mcp_channel_config_t publishable = release_config(&unpublished,
        &principal, 1000u);
    maelys_mcp_channel_t *never_published = NULL;
    release_activation(&activation);
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, &publishable,
        &never_published) == MAELYS_MCP_ERR_PROVIDER);
    ASSERT_TRUE(never_published == NULL);
    ASSERT_TRUE(atomic_load(&unpublished.calls) == 0);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    ASSERT_TRUE(atomic_load(&unpublished.calls) == 0);
    activation_context_clear(&activation);
    return 0;
}

typedef struct probe_destroy_context {
    maelys_mcp_runtime_t *runtime;
    release_probe_t *probe;
    maelys_mcp_result_t status;
} probe_destroy_context_t;

static void *probe_destroy_runtime_thread(void *opaque) {
    probe_destroy_context_t *context = opaque;
    context->status = maelys_mcp_runtime_destroy(context->runtime);
    atomic_store(&context->probe->runtime_destroy_returned, 1);
    return NULL;
}

/*
 * The path the callback was designed for, and the only one where getting it
 * wrong is not merely late but unsafe. The channel is detached with a provider
 * call still parked inside it, so:
 *
 *   - the release must NOT have happened when destroy_detached returned, which
 *     is the assertion that fails against the obvious wrong implementation
 *     (releasing from destroy, where the embedder's connection is);
 *   - it must happen on the worker's thread, not this one;
 *   - it must not overlap the provider call that is still holding the channel;
 *   - and it must have happened by the time maelys_mcp_runtime_destroy
 *     returns, because that call drains the detached channels and the release
 *     runs at the free, ahead of the retirement that wakes it.
 */
static int test_context_release_runs_once_on_the_detached_destroy(void) {
    int principal = 0;
    release_probe_t probe;
    release_probe_init(&probe, &principal);
    stuck_provider_t stuck;
    stuck_provider_init(&stuck);
    stuck.probe = &probe;
    maelys_mcp_runtime_t *runtime = new_stuck_runtime(&stuck, NULL);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_channel_config_t config = release_config(&probe, &principal,
        50u);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, &config, &channel) ==
        MAELYS_MCP_OK);
    json_t *call = stuck_call_request(1);
    ASSERT_TRUE(call != NULL);
    ASSERT_TRUE(maelys_mcp_channel_accept(channel, call) == MAELYS_MCP_OK);
    json_decref(call);
    stuck_provider_await_entries(&stuck, 1);

    ASSERT_TRUE(maelys_mcp_channel_destroy_detached(channel) ==
        MAELYS_MCP_ERR_TIMEOUT);
    /* The context is still reachable: a worker is holding the channel. */
    ASSERT_TRUE(atomic_load(&probe.calls) == 0);

    probe_destroy_context_t destroy = { .runtime = runtime, .probe = &probe,
        .status = MAELYS_MCP_OK };
    pthread_t thread;
    ASSERT_TRUE(pthread_create(&thread, NULL, probe_destroy_runtime_thread,
        &destroy) == 0);
    for (int slept = 0; slept < 10; ++slept) {
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
        nanosleep(&pause, NULL);
    }
    /* Still nothing: runtime_destroy is waiting for the detached channel, and
     * the detached channel is waiting for the provider. */
    ASSERT_TRUE(atomic_load(&probe.calls) == 0);

    stuck_provider_release(&stuck);
    ASSERT_TRUE(pthread_join(thread, NULL) == 0);
    ASSERT_TRUE(destroy.status == MAELYS_MCP_OK);
    ASSERT_TRUE(atomic_load(&probe.calls) == 1);
    ASSERT_TRUE(release_probe_is_clean(&probe));
    ASSERT_TRUE(atomic_load(&probe.ran_off_the_creating_thread) == 1);
    stuck_provider_clear(&stuck);
    return 0;
}

int main(void) {
    static const maelys_test_case_t tests[] = {
        {"context release runs once on the synchronous destroy",
            test_context_release_runs_once_on_the_synchronous_destroy},
        {"context release runs once on the detached destroy",
            test_context_release_runs_once_on_the_detached_destroy},
        {"context release is not called when creation fails",
            test_context_release_is_not_called_when_creation_fails},
        {"detached destroy joins a finished worker and detaches a running one",
            test_detached_destroy_joins_a_finished_worker},
        {"detached destroy returns while a provider is stuck",
            test_detached_destroy_returns_while_a_provider_is_stuck},
        {"runtime destroy waits for a detached channel",
            test_runtime_destroy_waits_for_a_detached_channel},
        {"detached destroy frees from an inline operation",
            test_detached_destroy_frees_from_an_inline_operation},
        {"detached destroy frees inline when nothing is in flight",
            test_detached_destroy_frees_inline_when_nothing_is_in_flight},
        {"detached destroy after an explicit close",
            test_detached_destroy_after_an_explicit_close},
        {"two detached channels both drain",
            test_two_detached_channels_both_drain},
        {"same JSON-RPC id is isolated with exact fanout",
            test_same_id_isolation_and_exact_fanout},
        {"slow channel fault is local", test_slow_channel_fault_is_local},
        {"slow fanout channel does not skip peer",
            test_slow_fanout_does_not_skip_peer},
        {"close drains complete and wakes consumer",
            test_close_drains_complete_and_wakes_consumer},
        {"concurrent provider activation is unique",
            test_concurrent_activation_is_unique},
        {"activation failure is fail-closed",
            test_activation_failure_is_fail_closed},
        {"runtime destroy races blocked activation safely",
            test_destroy_races_blocked_activation},
        {"create after closed gate is defensive, not contractual",
            test_create_after_gate_is_hardening_not_contract},
        {"runtime destroy waits for multi-provider callbacks",
            test_runtime_destroy_waits_for_multi_provider_callbacks},
        {"close uses one absolute deadline",
            test_close_uses_one_absolute_deadline},
        {"destroy consumes a channel after bounded close failure",
            test_destroy_consumes_channel_after_bounded_close_failure},
        {"close during fanout keeps other channel live",
            test_close_during_fanout_keeps_other_channel_live},
        {"destroy during fanout keeps other channel live",
            test_destroy_during_fanout_keeps_other_channel_live},
        {"zero-channel event and argument contracts",
            test_zero_channel_event_and_argument_contracts},
        {"concurrent channels dispatch under fanout",
            test_concurrent_channels_dispatch_under_fanout},
        {"channel context accessor is bound, read-back and NULL-safe",
            test_channel_context_accessor},
        {"tools/list shares no schema node across channels",
            test_tools_list_shares_no_schema_across_channels}
    };
    return maelys_run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
