#include "maelys/mcp.h"
#include "src/internal/internal.h"
#include "tests/test_support.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct capture {
    pthread_mutex_t mutex;
    json_t *messages[1024];
    size_t count;
} capture_t;

static maelys_mcp_result_t capture_write(void *context, json_t *message) {
    capture_t *capture = context;
    json_t *copy = json_deep_copy(message);
    if (!copy) return MAELYS_MCP_ERR_MEMORY;
    pthread_mutex_lock(&capture->mutex);
    if (capture->count == sizeof(capture->messages) / sizeof(capture->messages[0])) {
        pthread_mutex_unlock(&capture->mutex);
        json_decref(copy);
        return MAELYS_MCP_ERR_IO;
    }
    capture->messages[capture->count++] = copy;
    pthread_mutex_unlock(&capture->mutex);
    return MAELYS_MCP_OK;
}

static maelys_mcp_runtime_t *runtime_with_outbox(
    capture_t *capture,
    maelys_mcp_outbox_t **outbox) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "subscriptions", .server_version = "0.6.0",
        .max_subscriptions = 4u
    };
    maelys_mcp_runtime_t *runtime = NULL;
    maelys_mcp_outbox_config_t outbox_config = {
        .max_messages = 64u, .max_bytes = 1024u * 1024u,
        .batch_size = 8u, .response_burst = 8u,
        .write = capture_write, .write_context = capture
    };
    if (pthread_mutex_init(&capture->mutex, NULL) != 0 ||
        maelys_mcp_runtime_create(&config, &runtime) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_TOOLS) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_RESOURCES) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_enable_module(runtime,
            MAELYS_MCP_MODULE_SUBSCRIPTIONS) != MAELYS_MCP_OK ||
        maelys_mcp_outbox_create(&outbox_config, outbox) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_attach_outbox(runtime, *outbox) != MAELYS_MCP_OK) {
        if (*outbox) maelys_mcp_outbox_destroy(*outbox, 0);
        maelys_mcp_runtime_destroy(runtime);
        pthread_mutex_destroy(&capture->mutex);
        return NULL;
    }
    return runtime;
}

static void cleanup(
    maelys_mcp_runtime_t *runtime,
    maelys_mcp_outbox_t *outbox,
    capture_t *capture) {
    maelys_mcp_runtime_detach_outbox(runtime);
    if (outbox) (void)maelys_mcp_outbox_destroy(outbox, 1);
    maelys_mcp_runtime_destroy(runtime);
    for (size_t index = 0; index < capture->count; ++index) {
        json_decref(capture->messages[index]);
    }
    pthread_mutex_destroy(&capture->mutex);
}

static json_t *modern_meta(void) {
    return json_pack("{s:s,s:{s:s,s:s},s:{}}",
        "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
        "io.modelcontextprotocol/clientInfo", "name", "subscription-test", "version", "1",
        "io.modelcontextprotocol/clientCapabilities");
}

static json_t *listen_request(json_t *id, json_t *notifications) {
    return json_pack("{s:s,s:o,s:s,s:{s:o,s:o}}",
        "jsonrpc", "2.0", "id", id, "method", "subscriptions/listen",
        "params", "notifications", notifications, "_meta", modern_meta());
}

static json_t *cancel_notification(json_t *id) {
    return json_pack("{s:s,s:s,s:{s:o}}",
        "jsonrpc", "2.0", "method", "notifications/cancelled",
        "params", "requestId", id);
}

static const char *method_of(json_t *message) {
    json_t *method = json_object_get(message, "method");
    return json_is_string(method) ? json_string_value(method) : NULL;
}

static json_t *subscription_id_of(json_t *message) {
    json_t *params = json_object_get(message, "params");
    json_t *result = json_object_get(message, "result");
    json_t *meta = json_is_object(params) ? json_object_get(params, "_meta") :
        (json_is_object(result) ? json_object_get(result, "_meta") : NULL);
    return json_is_object(meta) ?
        json_object_get(meta, "io.modelcontextprotocol/subscriptionId") : NULL;
}

static int test_negotiation_events_and_cancellation(void) {
    capture_t capture = {0};
    maelys_mcp_outbox_t *outbox = NULL;
    maelys_mcp_runtime_t *runtime = runtime_with_outbox(&capture, &outbox);
    ASSERT_TRUE(runtime != NULL);
    json_t *filter = json_pack("{s:b,s:b,s:b,s:[s,s,s]}",
        "toolsListChanged", 1, "promptsListChanged", 1,
        "resourcesListChanged", 1, "resourceSubscriptions",
        "HERMES://repository/course.mdx", "hermes://repository/course.mdx",
        "hermes://repository/assets");
    json_t *request = listen_request(json_string("stream-A"), filter);
    ASSERT_TRUE(request != NULL);
    ASSERT_TRUE(maelys_mcp_runtime_handle(runtime, request) == NULL);
    json_decref(request);

    ASSERT_TRUE(maelys_mcp_runtime_notify_resource_updated(runtime,
        "HERMES://repository/assets/logo.png") == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_notify_resource_updated(runtime,
        "hermes://repository/other") == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_notify_resources_list_changed(runtime) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_notify_tools_list_changed(runtime) == MAELYS_MCP_OK);

    request = cancel_notification(json_string("stream-A"));
    ASSERT_TRUE(maelys_mcp_runtime_handle(runtime, request) == NULL);
    json_decref(request);
    ASSERT_TRUE(maelys_mcp_runtime_notify_tools_list_changed(runtime) == MAELYS_MCP_OK);

    maelys_mcp_runtime_detach_outbox(runtime);
    ASSERT_TRUE(maelys_mcp_outbox_destroy(outbox, 1) == MAELYS_MCP_OK);
    outbox = NULL;
    ASSERT_TRUE(capture.count == 4u);
    ASSERT_TRUE(strcmp(method_of(capture.messages[0]),
        "notifications/subscriptions/acknowledged") == 0);
    ASSERT_TRUE(strcmp(json_string_value(subscription_id_of(capture.messages[0])),
        "stream-A") == 0);
    json_t *accepted = json_object_get(
        json_object_get(capture.messages[0], "params"), "notifications");
    ASSERT_TRUE(json_is_true(json_object_get(accepted, "toolsListChanged")));
    ASSERT_TRUE(json_is_true(json_object_get(accepted, "resourcesListChanged")));
    ASSERT_TRUE(json_object_get(accepted, "promptsListChanged") == NULL);
    ASSERT_TRUE(json_array_size(json_object_get(accepted, "resourceSubscriptions")) == 2u);
    ASSERT_TRUE(strcmp(method_of(capture.messages[1]),
        "notifications/resources/updated") == 0);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(
        json_object_get(capture.messages[1], "params"), "uri")),
        "hermes://repository/assets/logo.png") == 0);
    ASSERT_TRUE(strcmp(method_of(capture.messages[2]),
        "notifications/resources/list_changed") == 0);
    ASSERT_TRUE(strcmp(method_of(capture.messages[3]),
        "notifications/tools/list_changed") == 0);
    cleanup(runtime, NULL, &capture);
    return 0;
}

static int test_validation_duplicate_and_completion(void) {
    capture_t capture = {0};
    maelys_mcp_outbox_t *outbox = NULL;
    maelys_mcp_runtime_t *runtime = runtime_with_outbox(&capture, &outbox);
    ASSERT_TRUE(runtime != NULL);
    json_t *request = listen_request(json_integer(42),
        json_pack("{s:b}", "toolsListChanged", 1));
    ASSERT_TRUE(maelys_mcp_runtime_handle(runtime, request) == NULL);
    json_decref(request);

    request = listen_request(json_integer(42), json_object());
    json_t *response = maelys_mcp_runtime_handle(runtime, request);
    json_decref(request);
    ASSERT_TRUE(json_integer_value(json_object_get(
        json_object_get(response, "error"), "code")) == -32602);
    json_decref(response);

    request = listen_request(json_integer(43),
        json_pack("{s:[s]}", "resourceSubscriptions", "file:///tmp/%00.png"));
    response = maelys_mcp_runtime_handle(runtime, request);
    json_decref(request);
    ASSERT_TRUE(json_integer_value(json_object_get(
        json_object_get(response, "error"), "code")) == -32602);
    json_decref(response);

    ASSERT_TRUE(maelys_mcp_runtime_complete_subscriptions(runtime) == MAELYS_MCP_OK);
    maelys_mcp_runtime_detach_outbox(runtime);
    ASSERT_TRUE(maelys_mcp_outbox_destroy(outbox, 1) == MAELYS_MCP_OK);
    outbox = NULL;
    ASSERT_TRUE(capture.count == 2u);
    ASSERT_TRUE(strcmp(method_of(capture.messages[0]),
        "notifications/subscriptions/acknowledged") == 0);
    ASSERT_TRUE(json_integer_value(json_object_get(capture.messages[1], "id")) == 42);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(
        json_object_get(capture.messages[1], "result"), "resultType")),
        "complete") == 0);
    ASSERT_TRUE(json_integer_value(subscription_id_of(capture.messages[1])) == 42);
    cleanup(runtime, NULL, &capture);
    return 0;
}

static int test_capabilities_depend_on_module(void) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "caps", .server_version = "0.6.0"
    };
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(maelys_mcp_runtime_create(&config, &runtime) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_TOOLS) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_RESOURCES) == MAELYS_MCP_OK);
    json_t *capabilities = maelys_mcp_runtime_capabilities(runtime, 1);
    ASSERT_TRUE(json_is_false(json_object_get(json_object_get(capabilities, "tools"), "listChanged")));
    ASSERT_TRUE(json_is_false(json_object_get(json_object_get(capabilities, "resources"), "subscribe")));
    json_decref(capabilities);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(runtime,
        MAELYS_MCP_MODULE_SUBSCRIPTIONS) == MAELYS_MCP_OK);
    capabilities = maelys_mcp_runtime_capabilities(runtime, 1);
    ASSERT_TRUE(json_is_true(json_object_get(json_object_get(capabilities, "tools"), "listChanged")));
    ASSERT_TRUE(json_is_true(json_object_get(json_object_get(capabilities, "resources"), "subscribe")));
    ASSERT_TRUE(json_is_true(json_object_get(json_object_get(capabilities, "resources"), "listChanged")));
    json_decref(capabilities);
    capabilities = maelys_mcp_runtime_capabilities(runtime, 0);
    ASSERT_TRUE(json_is_false(json_object_get(json_object_get(capabilities, "tools"), "listChanged")));
    ASSERT_TRUE(json_is_false(json_object_get(json_object_get(capabilities, "resources"), "subscribe")));
    json_decref(capabilities);
    maelys_mcp_runtime_destroy(runtime);
    return 0;
}

typedef struct event_producer {
    maelys_mcp_runtime_t *runtime;
    atomic_int *failed;
} event_producer_t;

static void *produce_tool_events(void *opaque) {
    event_producer_t *producer = opaque;
    for (size_t index = 0; index < 100u; ++index) {
        if (maelys_mcp_runtime_notify_tools_list_changed(producer->runtime) !=
            MAELYS_MCP_OK) {
            atomic_store(producer->failed, 1);
            break;
        }
    }
    return NULL;
}

static int test_concurrent_producers_and_cancel(void) {
    capture_t capture = {0};
    maelys_mcp_outbox_t *outbox = NULL;
    maelys_mcp_runtime_t *runtime = runtime_with_outbox(&capture, &outbox);
    ASSERT_TRUE(runtime != NULL);
    json_t *request = listen_request(json_string("concurrent"),
        json_pack("{s:b}", "toolsListChanged", 1));
    ASSERT_TRUE(maelys_mcp_runtime_handle(runtime, request) == NULL);
    json_decref(request);

    atomic_int failed = 0;
    pthread_t threads[4];
    event_producer_t producers[4];
    for (size_t index = 0; index < 4u; ++index) {
        producers[index].runtime = runtime;
        producers[index].failed = &failed;
        ASSERT_TRUE(pthread_create(&threads[index], NULL,
            produce_tool_events, &producers[index]) == 0);
    }
    request = cancel_notification(json_string("concurrent"));
    ASSERT_TRUE(maelys_mcp_runtime_handle(runtime, request) == NULL);
    json_decref(request);
    for (size_t index = 0; index < 4u; ++index) {
        ASSERT_TRUE(pthread_join(threads[index], NULL) == 0);
    }
    ASSERT_TRUE(atomic_load(&failed) == 0);
    maelys_mcp_runtime_detach_outbox(runtime);
    ASSERT_TRUE(maelys_mcp_outbox_destroy(outbox, 1) == MAELYS_MCP_OK);
    outbox = NULL;
    ASSERT_TRUE(capture.count >= 1u);
    ASSERT_TRUE(strcmp(method_of(capture.messages[0]),
        "notifications/subscriptions/acknowledged") == 0);
    cleanup(runtime, NULL, &capture);
    return 0;
}

static int subscription_model_matches(
    maelys_mcp_runtime_t *runtime,
    const int active[4]) {
    size_t expected_count = 0u;
    for (size_t index = 0; index < 4u; ++index) {
        if (active[index]) expected_count++;
    }
    int seen[4] = {0};
    int matches = 1;
    pthread_mutex_lock(&runtime->subscriptions_mutex);
    if (runtime->subscription_count != expected_count) matches = 0;
    for (size_t index = 0; matches && index < runtime->subscription_count; ++index) {
        maelys_mcp_subscription_t *subscription = &runtime->subscriptions[index];
        if (!subscription->active || !json_is_integer(subscription->id)) {
            matches = 0;
            break;
        }
        json_int_t id = json_integer_value(subscription->id);
        if (id < 0 || id >= 4 || !active[(size_t)id] || seen[(size_t)id]) {
            matches = 0;
            break;
        }
        seen[(size_t)id] = 1;
    }
    pthread_mutex_unlock(&runtime->subscriptions_mutex);
    if (!matches) return 0;
    for (size_t index = 0; index < 4u; ++index) {
        if (seen[index] != active[index]) return 0;
    }
    return 1;
}

static int test_randomized_subscription_state_machine(void) {
    capture_t capture = {0};
    maelys_mcp_outbox_t *outbox = NULL;
    maelys_mcp_runtime_t *runtime = runtime_with_outbox(&capture, &outbox);
    ASSERT_TRUE(runtime != NULL);
    int active[4] = {0};
    uint32_t random_state = 0x9e3779b9u;
    for (size_t iteration = 0; iteration < 512u; ++iteration) {
        random_state = random_state * 1103515245u + 12345u;
        size_t id = (size_t)((random_state >> 8u) % 4u);
        unsigned int action = (random_state >> 16u) % 3u;
        if (action == 0u) {
            json_t *request = listen_request(json_integer((json_int_t)id),
                json_pack("{s:b}", "toolsListChanged", 1));
            ASSERT_TRUE(request != NULL);
            json_t *response = maelys_mcp_runtime_handle(runtime, request);
            json_decref(request);
            if (active[id]) {
                ASSERT_TRUE(response != NULL);
                ASSERT_TRUE(json_integer_value(json_object_get(
                    json_object_get(response, "error"), "code")) == -32602);
                json_decref(response);
            } else {
                ASSERT_TRUE(response == NULL);
                active[id] = 1;
            }
        } else if (action == 1u) {
            json_t *request = cancel_notification(json_integer((json_int_t)id));
            ASSERT_TRUE(request != NULL);
            ASSERT_TRUE(maelys_mcp_runtime_handle(runtime, request) == NULL);
            json_decref(request);
            active[id] = 0;
        } else {
            ASSERT_TRUE(maelys_mcp_runtime_notify_tools_list_changed(runtime) ==
                MAELYS_MCP_OK);
        }
        ASSERT_TRUE(subscription_model_matches(runtime, active));
    }
    ASSERT_TRUE(maelys_mcp_runtime_complete_subscriptions(runtime) == MAELYS_MCP_OK);
    maelys_mcp_runtime_detach_outbox(runtime);
    ASSERT_TRUE(maelys_mcp_outbox_destroy(outbox, 1) == MAELYS_MCP_OK);
    outbox = NULL;
    ASSERT_TRUE(capture.count > 0u);
    cleanup(runtime, NULL, &capture);
    return 0;
}

int main(void) {
    static const maelys_test_case_t tests[] = {
        {"subscription negotiation, event filters and cancellation",
            test_negotiation_events_and_cancellation},
        {"subscription validation, duplicate ids and completion",
            test_validation_duplicate_and_completion},
        {"subscription-dependent capability advertisement",
            test_capabilities_depend_on_module},
        {"concurrent event producers and cancellation",
            test_concurrent_producers_and_cancel},
        {"randomized subscription state machine",
            test_randomized_subscription_state_machine}
    };
    return maelys_run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
