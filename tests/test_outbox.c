#include "maelys/mcp.h"
#include "tests/test_support.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct capture {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int block_first;
    int entered;
    int released;
    int fail;
    long long values[1024];
    char kinds[1024];
    size_t count;
} capture_t;

static void capture_init(capture_t *capture) {
    memset(capture, 0, sizeof(*capture));
    pthread_mutex_init(&capture->mutex, NULL);
    pthread_cond_init(&capture->condition, NULL);
}

static void capture_clear(capture_t *capture) {
    pthread_cond_destroy(&capture->condition);
    pthread_mutex_destroy(&capture->mutex);
}

static maelys_mcp_result_t capture_write(void *context, json_t *message) {
    capture_t *capture = context;
    pthread_mutex_lock(&capture->mutex);
    if (capture->block_first && !capture->entered) {
        capture->entered = 1;
        pthread_cond_broadcast(&capture->condition);
        while (!capture->released) pthread_cond_wait(&capture->condition, &capture->mutex);
    }
    if (capture->fail) {
        pthread_mutex_unlock(&capture->mutex);
        return MAELYS_MCP_ERR_IO;
    }
    size_t index = capture->count++;
    json_t *kind = json_object_get(message, "kind");
    capture->kinds[index] = json_is_string(kind) ? json_string_value(kind)[0] : '?';
    capture->values[index] = json_integer_value(json_object_get(message, "value"));
    pthread_mutex_unlock(&capture->mutex);
    return MAELYS_MCP_OK;
}

static maelys_mcp_outbox_t *new_outbox(capture_t *capture, size_t burst) {
    maelys_mcp_outbox_config_t config = {
        .max_messages = 16u,
        .max_bytes = 1024u * 1024u,
        .batch_size = 64u,
        .response_burst = burst,
        .write = capture_write,
        .write_context = capture
    };
    maelys_mcp_outbox_t *outbox = NULL;
    return maelys_mcp_outbox_create(&config, &outbox) == MAELYS_MCP_OK ? outbox : NULL;
}

static json_t *message(const char *kind, long long value) {
    return json_pack("{s:s,s:I}", "kind", kind, "value", (json_int_t)value);
}

static int enqueue(
    maelys_mcp_outbox_t *outbox,
    const char *kind,
    long long value,
    maelys_mcp_outbox_class_t message_class,
    const char *key) {
    json_t *value_json = message(kind, value);
    if (!value_json) return 0;
    maelys_mcp_result_t status = maelys_mcp_outbox_enqueue_take(
        outbox, value_json, message_class, key);
    if (status != MAELYS_MCP_OK) json_decref(value_json);
    return status == MAELYS_MCP_OK;
}

static int test_fairness_and_causal_coalescence(void) {
    capture_t capture;
    capture_init(&capture);
    capture.block_first = 1;
    maelys_mcp_outbox_t *outbox = new_outbox(&capture, 8u);
    ASSERT_TRUE(outbox != NULL);
    ASSERT_TRUE(enqueue(outbox, "response", 0, MAELYS_MCP_OUTBOX_RESPONSE, NULL));
    pthread_mutex_lock(&capture.mutex);
    while (!capture.entered) pthread_cond_wait(&capture.condition, &capture.mutex);
    pthread_mutex_unlock(&capture.mutex);
    ASSERT_TRUE(enqueue(outbox, "notification", 1, MAELYS_MCP_OUTBOX_NOTIFICATION, "resource:A"));
    ASSERT_TRUE(enqueue(outbox, "notification", 10, MAELYS_MCP_OUTBOX_NOTIFICATION, "resource:B"));
    ASSERT_TRUE(enqueue(outbox, "notification", 2, MAELYS_MCP_OUTBOX_NOTIFICATION, "resource:A"));
    for (long long value = 1; value < 10; ++value) {
        ASSERT_TRUE(enqueue(outbox, "response", value, MAELYS_MCP_OUTBOX_RESPONSE, NULL));
    }
    maelys_mcp_outbox_stats_t before = {0};
    maelys_mcp_outbox_stats(outbox, &before);
    ASSERT_TRUE(before.enqueued == 13u && before.coalesced == 1u);
    pthread_mutex_lock(&capture.mutex);
    capture.released = 1;
    pthread_cond_broadcast(&capture.condition);
    pthread_mutex_unlock(&capture.mutex);
    ASSERT_TRUE(maelys_mcp_outbox_destroy(outbox, 1) == MAELYS_MCP_OK);
    ASSERT_TRUE(capture.count == 12u);
    ASSERT_TRUE(capture.kinds[0] == 'r' && capture.values[0] == 0);
    ASSERT_TRUE(capture.kinds[8] == 'n' && capture.values[8] == 10);
    ASSERT_TRUE(capture.kinds[11] == 'n' && capture.values[11] == 2);
    capture_clear(&capture);
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
        if (!enqueue(producer->outbox, "response", producer->offset + index,
            MAELYS_MCP_OUTBOX_RESPONSE, NULL)) {
            atomic_store(producer->failed, 1);
            break;
        }
    }
    return NULL;
}

static int test_multiple_producers_and_bounded_drain(void) {
    capture_t capture;
    capture_init(&capture);
    maelys_mcp_outbox_t *outbox = new_outbox(&capture, 8u);
    ASSERT_TRUE(outbox != NULL);
    pthread_t threads[4];
    producer_t producers[4];
    atomic_int failed = 0;
    for (size_t index = 0; index < 4u; ++index) {
        producers[index] = (producer_t){
            .outbox = outbox, .offset = (long long)index * 1000LL, .failed = &failed
        };
        ASSERT_TRUE(pthread_create(&threads[index], NULL, produce, &producers[index]) == 0);
    }
    for (size_t index = 0; index < 4u; ++index) pthread_join(threads[index], NULL);
    ASSERT_TRUE(atomic_load(&failed) == 0);
    ASSERT_TRUE(maelys_mcp_outbox_destroy(outbox, 1) == MAELYS_MCP_OK);
    ASSERT_TRUE(capture.count == 400u);
    capture_clear(&capture);
    return 0;
}

static int test_writer_failure_closes_queue(void) {
    capture_t capture;
    capture_init(&capture);
    capture.fail = 1;
    maelys_mcp_outbox_t *outbox = new_outbox(&capture, 8u);
    ASSERT_TRUE(outbox != NULL);
    ASSERT_TRUE(enqueue(outbox, "response", 1, MAELYS_MCP_OUTBOX_RESPONSE, NULL));
    ASSERT_TRUE(maelys_mcp_outbox_destroy(outbox, 1) == MAELYS_MCP_ERR_IO);
    capture_clear(&capture);
    return 0;
}

int main(void) {
    static const maelys_test_case_t tests[] = {
        {"fair scheduling and causal coalescence", test_fairness_and_causal_coalescence},
        {"multiple producers and bounded drain", test_multiple_producers_and_bounded_drain},
        {"writer failure closes queue", test_writer_failure_closes_queue}
    };
    return maelys_run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
