#include "maelys/mcp/outbox.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct outbox_node {
    json_t *message;
    char *key;
    size_t bytes;
    struct outbox_node *next;
} outbox_node_t;

typedef struct outbox_queue {
    outbox_node_t *head;
    outbox_node_t *tail;
    size_t count;
} outbox_queue_t;

struct maelys_mcp_outbox {
    pthread_mutex_t mutex;
    pthread_cond_t available;
    pthread_cond_t space;
    pthread_t writer;
    maelys_mcp_outbox_config_t config;
    outbox_queue_t responses;
    outbox_queue_t notifications;
    size_t queued_messages;
    size_t queued_bytes;
    size_t consecutive_responses;
    int accepting;
    int discard;
    int writer_started;
    maelys_mcp_result_t writer_status;
    atomic_ullong enqueued;
    atomic_ullong written;
    atomic_ullong coalesced;
};

static char *copy_string(const char *value) {
    if (!value || !*value) return NULL;
    size_t length = strlen(value) + 1u;
    char *copy = malloc(length);
    if (copy) memcpy(copy, value, length);
    return copy;
}

static size_t message_bytes(json_t *message) {
    size_t bytes = json_dumpb(message, NULL, 0u, JSON_COMPACT | JSON_SORT_KEYS);
    return bytes ? bytes + 1u : 0u;
}

static void release_node(outbox_node_t *node) {
    if (!node) return;
    json_decref(node->message);
    free(node->key);
    free(node);
}

static void queue_push(outbox_queue_t *queue, outbox_node_t *node) {
    node->next = NULL;
    if (queue->tail) queue->tail->next = node;
    else queue->head = node;
    queue->tail = node;
    queue->count++;
}

static outbox_node_t *queue_pop(outbox_queue_t *queue) {
    outbox_node_t *node = queue->head;
    if (!node) return NULL;
    queue->head = node->next;
    if (!queue->head) queue->tail = NULL;
    node->next = NULL;
    queue->count--;
    return node;
}

static outbox_node_t *find_notification(
    outbox_queue_t *queue,
    const char *key,
    outbox_node_t **out_previous) {
    outbox_node_t *previous = NULL;
    for (outbox_node_t *node = queue->head; node; node = node->next) {
        if (node->key && strcmp(node->key, key) == 0) {
            if (out_previous) *out_previous = previous;
            return node;
        }
        previous = node;
    }
    return NULL;
}

static void move_to_tail(
    outbox_queue_t *queue,
    outbox_node_t *node,
    outbox_node_t *previous) {
    if (queue->tail == node) return;
    if (previous) previous->next = node->next;
    else queue->head = node->next;
    node->next = NULL;
    queue->tail->next = node;
    queue->tail = node;
}

static outbox_node_t *select_next(maelys_mcp_outbox_t *outbox) {
    int choose_response = outbox->responses.head &&
        (!outbox->notifications.head ||
         outbox->consecutive_responses < outbox->config.response_burst);
    if (choose_response) {
        outbox->consecutive_responses++;
        return queue_pop(&outbox->responses);
    }
    if (outbox->notifications.head) {
        outbox->consecutive_responses = 0u;
        return queue_pop(&outbox->notifications);
    }
    outbox->consecutive_responses = 0u;
    return queue_pop(&outbox->responses);
}

static void clear_pending(maelys_mcp_outbox_t *outbox) {
    outbox_node_t *node = NULL;
    while ((node = queue_pop(&outbox->responses)) != NULL) release_node(node);
    while ((node = queue_pop(&outbox->notifications)) != NULL) release_node(node);
    outbox->queued_messages = 0u;
    outbox->queued_bytes = 0u;
}

static void *writer_main(void *opaque) {
    maelys_mcp_outbox_t *outbox = opaque;
    outbox_node_t **batch = calloc(outbox->config.batch_size, sizeof(*batch));
    if (!batch) {
        pthread_mutex_lock(&outbox->mutex);
        outbox->writer_status = MAELYS_MCP_ERR_MEMORY;
        outbox->accepting = 0;
        clear_pending(outbox);
        pthread_cond_broadcast(&outbox->space);
        pthread_mutex_unlock(&outbox->mutex);
        return NULL;
    }
    for (;;) {
        pthread_mutex_lock(&outbox->mutex);
        while (outbox->queued_messages == 0u && outbox->accepting) {
            pthread_cond_wait(&outbox->available, &outbox->mutex);
        }
        if (outbox->discard) clear_pending(outbox);
        if (!outbox->accepting && outbox->queued_messages == 0u) {
            pthread_mutex_unlock(&outbox->mutex);
            break;
        }
        size_t count = 0u;
        while (count < outbox->config.batch_size && outbox->queued_messages) {
            outbox_node_t *node = select_next(outbox);
            if (!node) break;
            batch[count++] = node;
            outbox->queued_messages--;
            outbox->queued_bytes -= node->bytes;
        }
        pthread_cond_broadcast(&outbox->space);
        pthread_mutex_unlock(&outbox->mutex);

        for (size_t index = 0; index < count; ++index) {
            maelys_mcp_result_t status = outbox->config.write(
                outbox->config.write_context, batch[index]->message);
            if (status == MAELYS_MCP_OK) atomic_fetch_add(&outbox->written, 1u);
            release_node(batch[index]);
            batch[index] = NULL;
            if (status != MAELYS_MCP_OK) {
                for (++index; index < count; ++index) {
                    release_node(batch[index]);
                    batch[index] = NULL;
                }
                pthread_mutex_lock(&outbox->mutex);
                outbox->writer_status = status;
                outbox->accepting = 0;
                outbox->discard = 1;
                clear_pending(outbox);
                pthread_cond_broadcast(&outbox->space);
                pthread_mutex_unlock(&outbox->mutex);
                free(batch);
                return NULL;
            }
        }
    }
    free(batch);
    return NULL;
}

maelys_mcp_result_t maelys_mcp_outbox_create(
    const maelys_mcp_outbox_config_t *config,
    maelys_mcp_outbox_t **out_outbox) {
    if (!config || !out_outbox || !config->write) return MAELYS_MCP_ERR_ARGUMENT;
    *out_outbox = NULL;
    maelys_mcp_outbox_t *outbox = calloc(1u, sizeof(*outbox));
    if (!outbox) return MAELYS_MCP_ERR_MEMORY;
    outbox->config = *config;
    if (!outbox->config.max_messages) outbox->config.max_messages = 256u;
    if (!outbox->config.max_bytes) outbox->config.max_bytes = 4u * 1024u * 1024u;
    if (!outbox->config.batch_size) outbox->config.batch_size = 32u;
    if (!outbox->config.response_burst) outbox->config.response_burst = 8u;
    outbox->accepting = 1;
    outbox->writer_status = MAELYS_MCP_OK;
    if (pthread_mutex_init(&outbox->mutex, NULL) != 0) goto failed;
    if (pthread_cond_init(&outbox->available, NULL) != 0) goto failed_mutex;
    if (pthread_cond_init(&outbox->space, NULL) != 0) goto failed_available;
    if (pthread_create(&outbox->writer, NULL, writer_main, outbox) != 0) goto failed_space;
    outbox->writer_started = 1;
    *out_outbox = outbox;
    return MAELYS_MCP_OK;

failed_space:
    pthread_cond_destroy(&outbox->space);
failed_available:
    pthread_cond_destroy(&outbox->available);
failed_mutex:
    pthread_mutex_destroy(&outbox->mutex);
failed:
    free(outbox);
    return MAELYS_MCP_ERR_IO;
}

maelys_mcp_result_t maelys_mcp_outbox_enqueue_take(
    maelys_mcp_outbox_t *outbox,
    json_t *message,
    maelys_mcp_outbox_class_t message_class,
    const char *coalesce_key) {
    if (!outbox || !message ||
        (message_class != MAELYS_MCP_OUTBOX_RESPONSE &&
         message_class != MAELYS_MCP_OUTBOX_NOTIFICATION) ||
        (coalesce_key && *coalesce_key && message_class != MAELYS_MCP_OUTBOX_NOTIFICATION)) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    size_t bytes = message_bytes(message);
    if (!bytes || bytes > outbox->config.max_bytes) return MAELYS_MCP_ERR_PROTOCOL;
    char *key = copy_string(coalesce_key);
    if (coalesce_key && *coalesce_key && !key) return MAELYS_MCP_ERR_MEMORY;
    outbox_node_t *created = NULL;
    pthread_mutex_lock(&outbox->mutex);
    for (;;) {
        if (!outbox->accepting || outbox->writer_status != MAELYS_MCP_OK) {
            pthread_mutex_unlock(&outbox->mutex);
            free(key);
            return outbox->writer_status == MAELYS_MCP_OK ?
                MAELYS_MCP_ERR_IO : outbox->writer_status;
        }
        outbox_node_t *previous = NULL;
        outbox_node_t *existing = key ? find_notification(
            &outbox->notifications, key, &previous) : NULL;
        if (existing) {
            size_t next_bytes = outbox->queued_bytes - existing->bytes + bytes;
            if (next_bytes <= outbox->config.max_bytes) {
                json_t *old = existing->message;
                existing->message = message;
                outbox->queued_bytes = next_bytes;
                existing->bytes = bytes;
                move_to_tail(&outbox->notifications, existing, previous);
                atomic_fetch_add(&outbox->enqueued, 1u);
                atomic_fetch_add(&outbox->coalesced, 1u);
                pthread_cond_signal(&outbox->available);
                pthread_mutex_unlock(&outbox->mutex);
                json_decref(old);
                free(key);
                return MAELYS_MCP_OK;
            }
        } else if (outbox->queued_messages < outbox->config.max_messages &&
                   outbox->queued_bytes + bytes <= outbox->config.max_bytes) {
            created = calloc(1u, sizeof(*created));
            if (!created) {
                pthread_mutex_unlock(&outbox->mutex);
                free(key);
                return MAELYS_MCP_ERR_MEMORY;
            }
            created->message = message;
            created->key = key;
            created->bytes = bytes;
            key = NULL;
            queue_push(message_class == MAELYS_MCP_OUTBOX_RESPONSE ?
                &outbox->responses : &outbox->notifications, created);
            outbox->queued_messages++;
            outbox->queued_bytes += bytes;
            atomic_fetch_add(&outbox->enqueued, 1u);
            pthread_cond_signal(&outbox->available);
            pthread_mutex_unlock(&outbox->mutex);
            return MAELYS_MCP_OK;
        }
        pthread_cond_wait(&outbox->space, &outbox->mutex);
    }
}

maelys_mcp_result_t maelys_mcp_outbox_destroy(
    maelys_mcp_outbox_t *outbox,
    int drain) {
    if (!outbox) return MAELYS_MCP_ERR_ARGUMENT;
    pthread_mutex_lock(&outbox->mutex);
    outbox->accepting = 0;
    outbox->discard = !drain;
    pthread_cond_broadcast(&outbox->available);
    pthread_cond_broadcast(&outbox->space);
    pthread_mutex_unlock(&outbox->mutex);
    if (outbox->writer_started) pthread_join(outbox->writer, NULL);
    pthread_mutex_lock(&outbox->mutex);
    clear_pending(outbox);
    maelys_mcp_result_t status = outbox->writer_status;
    pthread_mutex_unlock(&outbox->mutex);
    pthread_cond_destroy(&outbox->space);
    pthread_cond_destroy(&outbox->available);
    pthread_mutex_destroy(&outbox->mutex);
    free(outbox);
    return status;
}

void maelys_mcp_outbox_stats(
    maelys_mcp_outbox_t *outbox,
    maelys_mcp_outbox_stats_t *out_stats) {
    if (!outbox || !out_stats) return;
    out_stats->enqueued = atomic_load(&outbox->enqueued);
    out_stats->written = atomic_load(&outbox->written);
    out_stats->coalesced = atomic_load(&outbox->coalesced);
    pthread_mutex_lock(&outbox->mutex);
    out_stats->queued_messages = outbox->queued_messages;
    out_stats->queued_bytes = outbox->queued_bytes;
    pthread_mutex_unlock(&outbox->mutex);
}
