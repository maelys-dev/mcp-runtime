#include "maelys/mcp.h"
#include "src/internal/internal.h"
#include "tests/test_support.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static maelys_mcp_outbox_t *new_outbox(
    size_t max_messages,
    size_t max_bytes,
    size_t burst,
    unsigned int timeout_ms) {
    maelys_mcp_outbox_config_t config = {
        .max_messages = max_messages,
        .max_bytes = max_bytes,
        .response_burst = burst,
        .admission_timeout_ms = timeout_ms
    };
    maelys_mcp_outbox_t *outbox = NULL;
    return maelys_mcp_outbox_create(&config, &outbox) == MAELYS_MCP_OK ?
        outbox : NULL;
}

static json_t *message(const char *kind, long long value) {
    return json_pack("{s:s,s:I}", "kind", kind, "value", (json_int_t)value);
}

static size_t serialized_bytes(json_t *value) {
    size_t bytes = json_dumpb(value, NULL, 0u, JSON_COMPACT | JSON_SORT_KEYS);
    return bytes ? bytes + 1u : 0u;
}

static maelys_mcp_result_t enqueue(
    maelys_mcp_outbox_t *outbox,
    const char *kind,
    long long value,
    maelys_mcp_outbox_class_t message_class,
    const char *key) {
    json_t *value_json = message(kind, value);
    if (!value_json) return MAELYS_MCP_ERR_MEMORY;
    maelys_mcp_result_t status = maelys_mcp_outbox_enqueue_take(
        outbox, value_json, message_class, key);
    if (status != MAELYS_MCP_OK) json_decref(value_json);
    return status;
}

static long long value_of(json_t *message_json) {
    return (long long)json_integer_value(json_object_get(message_json, "value"));
}

static int drain_value(
    maelys_mcp_outbox_t *outbox,
    const char *expected_kind,
    long long expected_value) {
    json_t *value_json = NULL;
    maelys_mcp_result_t status = maelys_mcp_outbox_next(outbox, 1000u, &value_json);
    if (status != MAELYS_MCP_OK || !value_json) return 0;
    json_t *kind = json_object_get(value_json, "kind");
    int matches = json_is_string(kind) &&
        strcmp(json_string_value(kind), expected_kind) == 0 &&
        value_of(value_json) == expected_value;
    json_decref(value_json);
    return matches;
}

static int test_fairness_and_causal_coalescence(void) {
    maelys_mcp_outbox_t *outbox = new_outbox(16u, 1024u * 1024u, 8u, 100u);
    ASSERT_TRUE(outbox != NULL);
    ASSERT_TRUE(enqueue(outbox, "response", 0, MAELYS_MCP_OUTBOX_RESPONSE, NULL) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(enqueue(outbox, "notification", 1,
        MAELYS_MCP_OUTBOX_NOTIFICATION, "resource:A") == MAELYS_MCP_OK);
    ASSERT_TRUE(enqueue(outbox, "notification", 10,
        MAELYS_MCP_OUTBOX_NOTIFICATION, "resource:B") == MAELYS_MCP_OK);
    ASSERT_TRUE(enqueue(outbox, "notification", 2,
        MAELYS_MCP_OUTBOX_NOTIFICATION, "resource:A") == MAELYS_MCP_OK);
    for (long long value = 1; value < 10; ++value) {
        ASSERT_TRUE(enqueue(outbox, "response", value,
            MAELYS_MCP_OUTBOX_RESPONSE, NULL) == MAELYS_MCP_OK);
    }
    maelys_mcp_outbox_stats_t stats = {0};
    maelys_mcp_outbox_stats(outbox, &stats);
    ASSERT_TRUE(stats.enqueued == 13u);
    ASSERT_TRUE(stats.coalesced == 1u);
    ASSERT_TRUE(stats.queued_messages == 12u);
    for (long long value = 0; value < 8; ++value) {
        ASSERT_TRUE(drain_value(outbox, "response", value));
    }
    ASSERT_TRUE(drain_value(outbox, "notification", 10));
    ASSERT_TRUE(drain_value(outbox, "response", 8));
    ASSERT_TRUE(drain_value(outbox, "response", 9));
    ASSERT_TRUE(drain_value(outbox, "notification", 2));
    ASSERT_TRUE(maelys_mcp_outbox_close(outbox, 0) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_outbox_wait_drained(outbox, 100u) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_outbox_destroy(outbox) == MAELYS_MCP_OK);
    return 0;
}

typedef struct producer {
    maelys_mcp_outbox_t *outbox;
    long long offset;
    atomic_int *failed;
} producer_t;

static void *produce(void *opaque) {
    producer_t *producer = opaque;
    for (long long index = 0; index < 100; ++index) {
        if (enqueue(producer->outbox, "response", producer->offset + index,
            MAELYS_MCP_OUTBOX_RESPONSE, NULL) != MAELYS_MCP_OK) {
            atomic_store(producer->failed, 1);
            break;
        }
    }
    return NULL;
}

static int test_multiple_producers_and_bounded_drain(void) {
    maelys_mcp_outbox_t *outbox = new_outbox(512u, 4u * 1024u * 1024u, 8u, 1000u);
    ASSERT_TRUE(outbox != NULL);
    pthread_t threads[4];
    producer_t producers[4];
    atomic_int failed = 0;
    for (size_t index = 0; index < 4u; ++index) {
        producers[index] = (producer_t){
            .outbox = outbox,
            .offset = (long long)index * 1000LL,
            .failed = &failed
        };
        ASSERT_TRUE(pthread_create(&threads[index], NULL, produce, &producers[index]) == 0);
    }
    for (size_t index = 0; index < 4u; ++index) {
        ASSERT_TRUE(pthread_join(threads[index], NULL) == 0);
    }
    ASSERT_TRUE(atomic_load(&failed) == 0);
    for (size_t index = 0; index < 400u; ++index) {
        json_t *value_json = NULL;
        ASSERT_TRUE(maelys_mcp_outbox_next(outbox, 1000u, &value_json) == MAELYS_MCP_OK);
        ASSERT_TRUE(value_json != NULL);
        json_decref(value_json);
    }
    ASSERT_TRUE(maelys_mcp_outbox_close(outbox, 0) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_outbox_wait_drained(outbox, 100u) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_outbox_destroy(outbox) == MAELYS_MCP_OK);
    return 0;
}

typedef struct waiting_consumer {
    maelys_mcp_outbox_t *outbox;
    atomic_int status;
} waiting_consumer_t;

static void *wait_for_message(void *opaque) {
    waiting_consumer_t *consumer = opaque;
    json_t *message_json = NULL;
    maelys_mcp_result_t status = maelys_mcp_outbox_next(
        consumer->outbox, 5000u, &message_json);
    if (message_json) json_decref(message_json);
    atomic_store(&consumer->status, (int)status);
    return NULL;
}

static int test_admission_timeout_and_close_wakeup(void) {
    maelys_mcp_outbox_t *outbox = new_outbox(1u, 1024u, 1u, 20u);
    ASSERT_TRUE(outbox != NULL);
    ASSERT_TRUE(enqueue(outbox, "response", 1, MAELYS_MCP_OUTBOX_RESPONSE, NULL) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(enqueue(outbox, "response", 2, MAELYS_MCP_OUTBOX_RESPONSE, NULL) ==
        MAELYS_MCP_ERR_TIMEOUT);
    maelys_mcp_outbox_stats_t stats = {0};
    maelys_mcp_outbox_stats(outbox, &stats);
    ASSERT_TRUE(stats.rejected == 1u);
    ASSERT_TRUE(drain_value(outbox, "response", 1));

    waiting_consumer_t consumer = {.outbox = outbox};
    atomic_init(&consumer.status, (int)MAELYS_MCP_OK);
    pthread_t thread;
    ASSERT_TRUE(pthread_create(&thread, NULL, wait_for_message, &consumer) == 0);
    for (size_t attempt = 0; attempt < 1000u &&
         maelys_mcp_outbox_waiter_count(outbox) == 0u; ++attempt) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000 * 1000};
        nanosleep(&pause, NULL);
    }
    ASSERT_TRUE(maelys_mcp_outbox_waiter_count(outbox) == 1u);
    ASSERT_TRUE(maelys_mcp_outbox_close(outbox, 0) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_outbox_destroy(outbox) == MAELYS_MCP_OK);
    ASSERT_TRUE(pthread_join(thread, NULL) == 0);
    ASSERT_TRUE((maelys_mcp_result_t)atomic_load(&consumer.status) ==
        MAELYS_MCP_ERR_CLOSED);
    return 0;
}

static int test_randomized_coalescence_and_byte_accounting(void) {
    enum { KEY_COUNT = 8, ITERATIONS = 512 };
    maelys_mcp_outbox_t *outbox = new_outbox(16u, 4096u, 8u, 100u);
    ASSERT_TRUE(outbox != NULL);
    int active[KEY_COUNT] = {0};
    long long values[KEY_COUNT] = {0};
    size_t bytes_by_key[KEY_COUNT] = {0};
    size_t order[KEY_COUNT] = {0};
    size_t order_count = 0u;
    size_t expected_bytes = 0u;
    uint32_t random_state = 0x6d2b79f5u;
    for (long long iteration = 0; iteration < ITERATIONS; ++iteration) {
        random_state = random_state * 1664525u + 1013904223u;
        size_t key_index = (size_t)(random_state % KEY_COUNT);
        char key[32];
        ASSERT_TRUE(snprintf(key, sizeof(key), "resource:%zu", key_index) > 0);
        json_t *candidate = message("notification", iteration);
        ASSERT_TRUE(candidate != NULL);
        size_t candidate_bytes = serialized_bytes(candidate);
        ASSERT_TRUE(candidate_bytes > 0u);
        size_t previous_position = order_count;
        if (active[key_index]) {
            expected_bytes -= bytes_by_key[key_index];
            for (size_t index = 0; index < order_count; ++index) {
                if (order[index] == key_index) previous_position = index;
            }
            ASSERT_TRUE(previous_position < order_count);
            for (size_t index = previous_position; index + 1u < order_count; ++index) {
                order[index] = order[index + 1u];
            }
            order_count--;
        } else {
            active[key_index] = 1;
        }
        values[key_index] = iteration;
        bytes_by_key[key_index] = candidate_bytes;
        expected_bytes += candidate_bytes;
        order[order_count++] = key_index;
        ASSERT_TRUE(maelys_mcp_outbox_enqueue_take(outbox, candidate,
            MAELYS_MCP_OUTBOX_NOTIFICATION, key) == MAELYS_MCP_OK);
        maelys_mcp_outbox_stats_t stats = {0};
        maelys_mcp_outbox_stats(outbox, &stats);
        ASSERT_TRUE(stats.queued_messages == order_count);
        ASSERT_TRUE(stats.queued_bytes == expected_bytes);
    }
    maelys_mcp_outbox_stats_t stats = {0};
    maelys_mcp_outbox_stats(outbox, &stats);
    ASSERT_TRUE(stats.enqueued == ITERATIONS);
    ASSERT_TRUE(stats.coalesced == ITERATIONS - KEY_COUNT);
    for (size_t index = 0; index < order_count; ++index) {
        ASSERT_TRUE(drain_value(outbox, "notification", values[order[index]]));
    }
    ASSERT_TRUE(maelys_mcp_outbox_close(outbox, 0) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_outbox_wait_drained(outbox, 100u) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_outbox_destroy(outbox) == MAELYS_MCP_OK);
    return 0;
}

static int test_close_requires_bounded_drain(void) {
    maelys_mcp_outbox_t *outbox = new_outbox(2u, 1024u, 1u, 100u);
    ASSERT_TRUE(outbox != NULL);
    ASSERT_TRUE(enqueue(outbox, "response", 1, MAELYS_MCP_OUTBOX_RESPONSE, NULL) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_outbox_close(outbox, 0) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_outbox_wait_drained(outbox, 20u) ==
        MAELYS_MCP_ERR_TIMEOUT);
    ASSERT_TRUE(maelys_mcp_outbox_destroy(outbox) == MAELYS_MCP_ERR_STATE);
    ASSERT_TRUE(drain_value(outbox, "response", 1));
    json_t *none = NULL;
    ASSERT_TRUE(maelys_mcp_outbox_next(outbox, 20u, &none) == MAELYS_MCP_ERR_CLOSED);
    ASSERT_TRUE(none == NULL);
    ASSERT_TRUE(maelys_mcp_outbox_wait_drained(outbox, 20u) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_outbox_destroy(outbox) == MAELYS_MCP_OK);
    return 0;
}

int main(void) {
    static const maelys_test_case_t tests[] = {
        {"fair scheduling and causal coalescence", test_fairness_and_causal_coalescence},
        {"multiple producers and bounded drain", test_multiple_producers_and_bounded_drain},
        {"admission timeout and close wakeup", test_admission_timeout_and_close_wakeup},
        {"randomized coalescence and byte accounting",
            test_randomized_coalescence_and_byte_accounting},
        {"close requires bounded drain", test_close_requires_bounded_drain}
    };
    return maelys_run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
