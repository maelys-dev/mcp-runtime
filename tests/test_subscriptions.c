#include "maelys/mcp.h"
#include "src/internal/internal.h"
#include "tests/test_support.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
    maelys_mcp_runtime_t *runtime;
    maelys_mcp_channel_t *channel;
} fixture_t;

static int fixture_create(fixture_t *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    maelys_mcp_runtime_config_t config = {
        .server_name = "subscriptions", .server_version = "0.10.0",
        .max_subscriptions = 4u
    };
    maelys_mcp_channel_config_t channel_config = {
        .max_messages = 64u, .max_bytes = 1024u * 1024u,
        .response_burst = 8u, .admission_timeout_ms = 1000u,
        .close_timeout_ms = 1000u
    };
    if (maelys_mcp_runtime_create(&config, &fixture->runtime) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_enable_module(fixture->runtime,
            MAELYS_MCP_MODULE_TOOLS) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_enable_module(fixture->runtime,
            MAELYS_MCP_MODULE_RESOURCES) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_enable_module(fixture->runtime,
            MAELYS_MCP_MODULE_SUBSCRIPTIONS) != MAELYS_MCP_OK ||
        maelys_mcp_channel_create(fixture->runtime, &channel_config,
            &fixture->channel) != MAELYS_MCP_OK) {
        if (fixture->channel) (void)maelys_mcp_channel_destroy(fixture->channel);
        if (fixture->runtime) {
            maelys_mcp_result_t destroy_status =
                maelys_mcp_runtime_destroy(fixture->runtime);
            (void)destroy_status;
        }
        return 0;
    }
    return 1;
}

static int fixture_destroy(fixture_t *fixture) {
    if (maelys_mcp_channel_destroy(fixture->channel) != MAELYS_MCP_OK) return 0;
    return maelys_mcp_runtime_destroy(fixture->runtime) == MAELYS_MCP_OK;
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

static maelys_mcp_result_t handle(
    maelys_mcp_channel_t *channel,
    json_t *request) {
    maelys_mcp_result_t status = maelys_mcp_channel_handle(channel, request);
    json_decref(request);
    return status;
}

static json_t *next_message(maelys_mcp_channel_t *channel) {
    json_t *message = NULL;
    return maelys_mcp_channel_next(channel, 1000u, &message) == MAELYS_MCP_OK ?
        message : NULL;
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

static json_t *next_listen_outcome(
    maelys_mcp_channel_t *channel,
    json_t *expected_id) {
    for (size_t attempt = 0; attempt < 64u; ++attempt) {
        json_t *message = next_message(channel);
        if (!message) return NULL;
        json_t *response_id = json_object_get(message, "id");
        json_t *subscription_id = subscription_id_of(message);
        if ((response_id && json_equal(response_id, expected_id)) ||
            (subscription_id && json_equal(subscription_id, expected_id) &&
             method_of(message) && strcmp(method_of(message),
                "notifications/subscriptions/acknowledged") == 0)) {
            return message;
        }
        json_decref(message);
    }
    return NULL;
}

static int test_negotiation_events_and_cancellation(void) {
    fixture_t fixture;
    ASSERT_TRUE(fixture_create(&fixture));
    json_t *filter = json_pack("{s:b,s:b,s:b,s:[s,s,s]}",
        "toolsListChanged", 1, "promptsListChanged", 1,
        "resourcesListChanged", 1, "resourceSubscriptions",
        "HERMES://repository/course.mdx", "hermes://repository/course.mdx",
        "hermes://repository/assets");
    ASSERT_TRUE(handle(fixture.channel,
        listen_request(json_string("stream-A"), filter)) == MAELYS_MCP_OK);
    json_t *messages[4] = {next_message(fixture.channel), NULL, NULL, NULL};
    ASSERT_TRUE(messages[0] != NULL);

    ASSERT_TRUE(maelys_mcp_runtime_notify_resource_updated(fixture.runtime,
        "HERMES://repository/assets/logo.png") == MAELYS_MCP_OK);
    messages[1] = next_message(fixture.channel);
    ASSERT_TRUE(messages[1] != NULL);
    ASSERT_TRUE(maelys_mcp_runtime_notify_resource_updated(fixture.runtime,
        "hermes://repository/other") == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_notify_resources_list_changed(fixture.runtime) ==
        MAELYS_MCP_OK);
    messages[2] = next_message(fixture.channel);
    ASSERT_TRUE(messages[2] != NULL);
    ASSERT_TRUE(maelys_mcp_runtime_notify_tools_list_changed(fixture.runtime) ==
        MAELYS_MCP_OK);
    messages[3] = next_message(fixture.channel);
    ASSERT_TRUE(messages[3] != NULL);

    ASSERT_TRUE(handle(fixture.channel,
        cancel_notification(json_string("stream-A"))) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_notify_tools_list_changed(fixture.runtime) ==
        MAELYS_MCP_OK);
    json_t *none = NULL;
    ASSERT_TRUE(maelys_mcp_channel_next(fixture.channel, 20u, &none) ==
        MAELYS_MCP_ERR_TIMEOUT);

    ASSERT_TRUE(strcmp(method_of(messages[0]),
        "notifications/subscriptions/acknowledged") == 0);
    ASSERT_TRUE(strcmp(json_string_value(subscription_id_of(messages[0])),
        "stream-A") == 0);
    json_t *accepted = json_object_get(json_object_get(messages[0], "params"),
        "notifications");
    ASSERT_TRUE(json_is_true(json_object_get(accepted, "toolsListChanged")));
    ASSERT_TRUE(json_is_true(json_object_get(accepted, "resourcesListChanged")));
    ASSERT_TRUE(json_object_get(accepted, "promptsListChanged") == NULL);
    ASSERT_TRUE(json_array_size(json_object_get(accepted,
        "resourceSubscriptions")) == 2u);
    ASSERT_TRUE(strcmp(method_of(messages[1]), "notifications/resources/updated") == 0);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(
        json_object_get(messages[1], "params"), "uri")),
        "hermes://repository/assets/logo.png") == 0);
    ASSERT_TRUE(strcmp(method_of(messages[2]),
        "notifications/resources/list_changed") == 0);
    ASSERT_TRUE(strcmp(method_of(messages[3]), "notifications/tools/list_changed") == 0);
    for (size_t index = 0; index < 4u; ++index) json_decref(messages[index]);
    ASSERT_TRUE(fixture_destroy(&fixture));
    return 0;
}

static int test_validation_duplicate_and_completion(void) {
    fixture_t fixture;
    ASSERT_TRUE(fixture_create(&fixture));
    ASSERT_TRUE(handle(fixture.channel, listen_request(json_integer(42),
        json_pack("{s:b}", "toolsListChanged", 1))) == MAELYS_MCP_OK);
    json_t *ack = next_message(fixture.channel);
    ASSERT_TRUE(ack != NULL);

    ASSERT_TRUE(handle(fixture.channel,
        listen_request(json_integer(42), json_object())) == MAELYS_MCP_OK);
    json_t *response = next_message(fixture.channel);
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(json_integer_value(json_object_get(
        json_object_get(response, "error"), "code")) == -32602);
    json_decref(response);

    ASSERT_TRUE(handle(fixture.channel, listen_request(json_integer(43),
        json_pack("{s:[s]}", "resourceSubscriptions",
            "file:///tmp/%00.png"))) == MAELYS_MCP_OK);
    response = next_message(fixture.channel);
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(json_integer_value(json_object_get(
        json_object_get(response, "error"), "code")) == -32602);
    json_decref(response);

    ASSERT_TRUE(maelys_mcp_channel_complete_subscriptions(fixture.channel) ==
        MAELYS_MCP_OK);
    json_t *complete = next_message(fixture.channel);
    ASSERT_TRUE(complete != NULL);
    ASSERT_TRUE(strcmp(method_of(ack), "notifications/subscriptions/acknowledged") == 0);
    ASSERT_TRUE(json_integer_value(json_object_get(complete, "id")) == 42);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(
        json_object_get(complete, "result"), "resultType")), "complete") == 0);
    ASSERT_TRUE(json_integer_value(subscription_id_of(complete)) == 42);
    json_decref(ack);
    json_decref(complete);
    ASSERT_TRUE(fixture_destroy(&fixture));
    return 0;
}

static int test_capabilities_depend_on_module(void) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "caps", .server_version = "0.10.0"
    };
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(maelys_mcp_runtime_create(&config, &runtime) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(runtime,
        MAELYS_MCP_MODULE_TOOLS) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(runtime,
        MAELYS_MCP_MODULE_RESOURCES) == MAELYS_MCP_OK);
    json_t *capabilities = maelys_mcp_runtime_capabilities(runtime, 1);
    ASSERT_TRUE(json_is_false(json_object_get(json_object_get(capabilities,
        "tools"), "listChanged")));
    ASSERT_TRUE(json_is_false(json_object_get(json_object_get(capabilities,
        "resources"), "subscribe")));
    json_decref(capabilities);
    ASSERT_TRUE(maelys_mcp_runtime_enable_module(runtime,
        MAELYS_MCP_MODULE_SUBSCRIPTIONS) == MAELYS_MCP_OK);
    capabilities = maelys_mcp_runtime_capabilities(runtime, 1);
    ASSERT_TRUE(json_is_true(json_object_get(json_object_get(capabilities,
        "tools"), "listChanged")));
    ASSERT_TRUE(json_is_true(json_object_get(json_object_get(capabilities,
        "resources"), "subscribe")));
    ASSERT_TRUE(json_is_true(json_object_get(json_object_get(capabilities,
        "resources"), "listChanged")));
    json_decref(capabilities);
    capabilities = maelys_mcp_runtime_capabilities(runtime, 0);
    ASSERT_TRUE(json_is_false(json_object_get(json_object_get(capabilities,
        "tools"), "listChanged")));
    ASSERT_TRUE(json_is_false(json_object_get(json_object_get(capabilities,
        "resources"), "subscribe")));
    json_decref(capabilities);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
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
    fixture_t fixture;
    ASSERT_TRUE(fixture_create(&fixture));
    ASSERT_TRUE(handle(fixture.channel, listen_request(json_string("concurrent"),
        json_pack("{s:b}", "toolsListChanged", 1))) == MAELYS_MCP_OK);
    json_t *ack = next_message(fixture.channel);
    ASSERT_TRUE(ack != NULL);
    json_decref(ack);

    atomic_int failed = 0;
    pthread_t threads[4];
    event_producer_t producers[4];
    for (size_t index = 0; index < 4u; ++index) {
        producers[index].runtime = fixture.runtime;
        producers[index].failed = &failed;
        ASSERT_TRUE(pthread_create(&threads[index], NULL,
            produce_tool_events, &producers[index]) == 0);
    }
    ASSERT_TRUE(handle(fixture.channel,
        cancel_notification(json_string("concurrent"))) == MAELYS_MCP_OK);
    for (size_t index = 0; index < 4u; ++index) {
        ASSERT_TRUE(pthread_join(threads[index], NULL) == 0);
    }
    ASSERT_TRUE(atomic_load(&failed) == 0);
    size_t event_count = 0u;
    for (;;) {
        json_t *event = NULL;
        maelys_mcp_result_t status = maelys_mcp_channel_next(
            fixture.channel, 20u, &event);
        if (status == MAELYS_MCP_ERR_TIMEOUT) break;
        ASSERT_TRUE(status == MAELYS_MCP_OK && event != NULL);
        event_count++;
        json_decref(event);
    }
    ASSERT_TRUE(event_count <= 1u);
    ASSERT_TRUE(fixture_destroy(&fixture));
    return 0;
}

static int subscription_model_matches(
    maelys_mcp_channel_t *channel,
    const int active[4]) {
    size_t expected_count = 0u;
    for (size_t index = 0; index < 4u; ++index) {
        if (active[index]) expected_count++;
    }
    int seen[4] = {0};
    int matches = 1;
    pthread_mutex_lock(&channel->mutex);
    if (channel->subscription_count != expected_count) matches = 0;
    for (size_t index = 0; matches && index < channel->subscription_count; ++index) {
        maelys_mcp_subscription_t *subscription = &channel->subscriptions[index];
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
    pthread_mutex_unlock(&channel->mutex);
    if (!matches) return 0;
    for (size_t index = 0; index < 4u; ++index) {
        if (seen[index] != active[index]) return 0;
    }
    return 1;
}

static int test_randomized_subscription_state_machine(void) {
    fixture_t fixture;
    ASSERT_TRUE(fixture_create(&fixture));
    int active[4] = {0};
    uint32_t random_state = 0x9e3779b9u;
    for (size_t iteration = 0; iteration < 512u; ++iteration) {
        random_state = random_state * 1103515245u + 12345u;
        size_t id = (size_t)((random_state >> 8u) % 4u);
        unsigned int action = (random_state >> 16u) % 3u;
        if (action == 0u) {
            json_t *request_id = json_integer((json_int_t)id);
            ASSERT_TRUE(request_id != NULL);
            json_incref(request_id);
            ASSERT_TRUE(handle(fixture.channel, listen_request(request_id,
                json_pack("{s:b}", "toolsListChanged", 1))) == MAELYS_MCP_OK);
            json_t *response = next_listen_outcome(fixture.channel, request_id);
            json_decref(request_id);
            ASSERT_TRUE(response != NULL);
            if (active[id]) {
                ASSERT_TRUE(json_integer_value(json_object_get(
                    json_object_get(response, "error"), "code")) == -32602);
            } else {
                ASSERT_TRUE(strcmp(method_of(response),
                    "notifications/subscriptions/acknowledged") == 0);
                active[id] = 1;
            }
            json_decref(response);
        } else if (action == 1u) {
            ASSERT_TRUE(handle(fixture.channel, cancel_notification(
                json_integer((json_int_t)id))) == MAELYS_MCP_OK);
            active[id] = 0;
        } else {
            ASSERT_TRUE(maelys_mcp_runtime_notify_tools_list_changed(
                fixture.runtime) == MAELYS_MCP_OK);
        }
        ASSERT_TRUE(subscription_model_matches(fixture.channel, active));
    }
    ASSERT_TRUE(maelys_mcp_channel_complete_subscriptions(fixture.channel) ==
        MAELYS_MCP_OK);
    size_t messages = 0u;
    for (;;) {
        json_t *message = NULL;
        maelys_mcp_result_t status = maelys_mcp_channel_next(
            fixture.channel, 20u, &message);
        if (status == MAELYS_MCP_ERR_TIMEOUT) break;
        ASSERT_TRUE(status == MAELYS_MCP_OK && message != NULL);
        messages++;
        json_decref(message);
    }
    ASSERT_TRUE(messages > 0u);
    ASSERT_TRUE(fixture_destroy(&fixture));
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
