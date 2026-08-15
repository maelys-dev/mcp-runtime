#include "maelys/mcp.h"
#include "src/internal/internal.h"
#include "tests/test_support.h"

#include <pthread.h>
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
        pthread_mutex_lock(&runtime->lifecycle_mutex);
        size_t count = runtime->channel_creators_inflight;
        pthread_mutex_unlock(&runtime->lifecycle_mutex);
        if (count == expected) return 1;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000 * 1000};
        nanosleep(&pause, NULL);
    }
    return 0;
}

static int wait_for_shutdown_request(maelys_mcp_runtime_t *runtime) {
    for (size_t attempt = 0; attempt < 1000u; ++attempt) {
        pthread_mutex_lock(&runtime->lifecycle_mutex);
        int requested = runtime->shutdown_requested;
        pthread_mutex_unlock(&runtime->lifecycle_mutex);
        if (requested) return 1;
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
    ASSERT_TRUE(wait_for_shutdown_request(runtime));
    release_activation(&activation);
    for (size_t index = 0; index < CREATOR_COUNT; ++index) {
        ASSERT_TRUE(pthread_join(creator_threads[index], NULL) == 0);
        ASSERT_TRUE(creators[index].status == MAELYS_MCP_ERR_STATE);
        ASSERT_TRUE(creators[index].channel == NULL);
    }
    ASSERT_TRUE(pthread_join(destroyer_thread, NULL) == 0);
    ASSERT_TRUE(destroyer.status == MAELYS_MCP_OK);
    ASSERT_TRUE(activation.calls == 1);
    activation_context_clear(&activation);
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

int main(void) {
    static const maelys_test_case_t tests[] = {
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
        {"close uses one absolute deadline",
            test_close_uses_one_absolute_deadline},
        {"close during fanout keeps other channel live",
            test_close_during_fanout_keeps_other_channel_live},
        {"zero-channel event and argument contracts",
            test_zero_channel_event_and_argument_contracts}
    };
    return maelys_run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
