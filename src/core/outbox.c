#include "src/internal/internal.h"
#include "maelys/mcp/outbox.h"

#include <errno.h>
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
    maelys_mcp_outbox_config_t config;
    outbox_queue_t responses;
    outbox_queue_t notifications;
    size_t queued_messages;
    size_t queued_bytes;
    size_t consecutive_responses;
    int accepting;
    atomic_ullong enqueued;
    atomic_ullong dequeued;
    atomic_ullong coalesced;
    atomic_ullong rejected;
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
    if (node->message) json_decref(node->message);
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

maelys_mcp_result_t maelys_mcp_outbox_create(
    const maelys_mcp_outbox_config_t *config,
    maelys_mcp_outbox_t **out_outbox) {
    if (!config || !out_outbox) return MAELYS_MCP_ERR_ARGUMENT;
    *out_outbox = NULL;
    maelys_mcp_outbox_t *outbox = calloc(1u, sizeof(*outbox));
    if (!outbox) return MAELYS_MCP_ERR_MEMORY;
    outbox->config = *config;
    if (!outbox->config.max_messages) outbox->config.max_messages = 256u;
    if (!outbox->config.max_bytes) outbox->config.max_bytes = 4u * 1024u * 1024u;
    if (!outbox->config.response_burst) outbox->config.response_burst = 8u;
    if (!outbox->config.admission_timeout_ms) {
        outbox->config.admission_timeout_ms = 5000u;
    }
    outbox->accepting = 1;
    if (pthread_mutex_init(&outbox->mutex, NULL) != 0) goto failed;
    if (maelys_mcp_cond_init_monotonic(&outbox->available) != 0) goto failed_mutex;
    if (maelys_mcp_cond_init_monotonic(&outbox->space) != 0) goto failed_available;
    *out_outbox = outbox;
    return MAELYS_MCP_OK;

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
        (coalesce_key && *coalesce_key &&
         message_class != MAELYS_MCP_OUTBOX_NOTIFICATION)) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    size_t bytes = message_bytes(message);
    if (!bytes || bytes > outbox->config.max_bytes) {
        atomic_fetch_add(&outbox->rejected, 1u);
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    char *key = copy_string(coalesce_key);
    if (coalesce_key && *coalesce_key && !key) return MAELYS_MCP_ERR_MEMORY;
    uint64_t deadline_ms = 0u;
    if (maelys_mcp_monotonic_deadline(
        outbox->config.admission_timeout_ms, &deadline_ms) != 0) {
        free(key);
        return MAELYS_MCP_ERR_IO;
    }
    pthread_mutex_lock(&outbox->mutex);
    for (;;) {
        if (!outbox->accepting) {
            pthread_mutex_unlock(&outbox->mutex);
            free(key);
            atomic_fetch_add(&outbox->rejected, 1u);
            return MAELYS_MCP_ERR_CLOSED;
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
            outbox_node_t *created = calloc(1u, sizeof(*created));
            if (!created) {
                pthread_mutex_unlock(&outbox->mutex);
                free(key);
                atomic_fetch_add(&outbox->rejected, 1u);
                return MAELYS_MCP_ERR_MEMORY;
            }
            created->message = message;
            created->key = key;
            created->bytes = bytes;
            queue_push(message_class == MAELYS_MCP_OUTBOX_RESPONSE ?
                &outbox->responses : &outbox->notifications, created);
            outbox->queued_messages++;
            outbox->queued_bytes += bytes;
            atomic_fetch_add(&outbox->enqueued, 1u);
            pthread_cond_signal(&outbox->available);
            pthread_mutex_unlock(&outbox->mutex);
            return MAELYS_MCP_OK;
        }
        int waited = maelys_mcp_cond_wait_until(
            &outbox->space, &outbox->mutex, deadline_ms);
        if (waited == ETIMEDOUT) {
            pthread_mutex_unlock(&outbox->mutex);
            free(key);
            atomic_fetch_add(&outbox->rejected, 1u);
            return MAELYS_MCP_ERR_TIMEOUT;
        }
        if (waited != 0) {
            pthread_mutex_unlock(&outbox->mutex);
            free(key);
            atomic_fetch_add(&outbox->rejected, 1u);
            return MAELYS_MCP_ERR_IO;
        }
    }
}

maelys_mcp_result_t maelys_mcp_outbox_next(
    maelys_mcp_outbox_t *outbox,
    unsigned int timeout_ms,
    json_t **out_message) {
    if (!outbox || !timeout_ms || !out_message) return MAELYS_MCP_ERR_ARGUMENT;
    *out_message = NULL;
    uint64_t deadline_ms = 0u;
    if (maelys_mcp_monotonic_deadline(timeout_ms, &deadline_ms) != 0) {
        return MAELYS_MCP_ERR_IO;
    }
    pthread_mutex_lock(&outbox->mutex);
    while (outbox->queued_messages == 0u && outbox->accepting) {
        int waited = maelys_mcp_cond_wait_until(
            &outbox->available, &outbox->mutex, deadline_ms);
        if (waited == ETIMEDOUT) {
            pthread_mutex_unlock(&outbox->mutex);
            return MAELYS_MCP_ERR_TIMEOUT;
        }
        if (waited != 0) {
            pthread_mutex_unlock(&outbox->mutex);
            return MAELYS_MCP_ERR_IO;
        }
    }
    if (outbox->queued_messages == 0u) {
        pthread_mutex_unlock(&outbox->mutex);
        return MAELYS_MCP_ERR_CLOSED;
    }
    outbox_node_t *node = select_next(outbox);
    if (!node) {
        pthread_mutex_unlock(&outbox->mutex);
        return MAELYS_MCP_ERR_IO;
    }
    outbox->queued_messages--;
    outbox->queued_bytes -= node->bytes;
    *out_message = node->message;
    node->message = NULL;
    atomic_fetch_add(&outbox->dequeued, 1u);
    pthread_cond_broadcast(&outbox->space);
    pthread_mutex_unlock(&outbox->mutex);
    release_node(node);
    return MAELYS_MCP_OK;
}

maelys_mcp_result_t maelys_mcp_outbox_close(
    maelys_mcp_outbox_t *outbox,
    int discard) {
    if (!outbox) return MAELYS_MCP_ERR_ARGUMENT;
    pthread_mutex_lock(&outbox->mutex);
    outbox->accepting = 0;
    if (discard) clear_pending(outbox);
    pthread_cond_broadcast(&outbox->available);
    pthread_cond_broadcast(&outbox->space);
    pthread_mutex_unlock(&outbox->mutex);
    return MAELYS_MCP_OK;
}

maelys_mcp_result_t maelys_mcp_outbox_wait_drained(
    maelys_mcp_outbox_t *outbox,
    unsigned int timeout_ms) {
    if (!outbox || !timeout_ms) return MAELYS_MCP_ERR_ARGUMENT;
    uint64_t deadline_ms = 0u;
    if (maelys_mcp_monotonic_deadline(timeout_ms, &deadline_ms) != 0) {
        return MAELYS_MCP_ERR_IO;
    }
    pthread_mutex_lock(&outbox->mutex);
    if (outbox->accepting) {
        pthread_mutex_unlock(&outbox->mutex);
        return MAELYS_MCP_ERR_STATE;
    }
    while (outbox->queued_messages != 0u) {
        int waited = maelys_mcp_cond_wait_until(
            &outbox->space, &outbox->mutex, deadline_ms);
        if (waited == ETIMEDOUT) {
            pthread_mutex_unlock(&outbox->mutex);
            return MAELYS_MCP_ERR_TIMEOUT;
        }
        if (waited != 0) {
            pthread_mutex_unlock(&outbox->mutex);
            return MAELYS_MCP_ERR_IO;
        }
    }
    pthread_mutex_unlock(&outbox->mutex);
    return MAELYS_MCP_OK;
}

maelys_mcp_result_t maelys_mcp_outbox_destroy(maelys_mcp_outbox_t *outbox) {
    if (!outbox) return MAELYS_MCP_ERR_ARGUMENT;
    pthread_mutex_lock(&outbox->mutex);
    if (outbox->accepting || outbox->queued_messages != 0u) {
        pthread_mutex_unlock(&outbox->mutex);
        return MAELYS_MCP_ERR_STATE;
    }
    pthread_mutex_unlock(&outbox->mutex);
    pthread_cond_destroy(&outbox->space);
    pthread_cond_destroy(&outbox->available);
    pthread_mutex_destroy(&outbox->mutex);
    free(outbox);
    return MAELYS_MCP_OK;
}

void maelys_mcp_outbox_stats(
    maelys_mcp_outbox_t *outbox,
    maelys_mcp_outbox_stats_t *out_stats) {
    if (!outbox || !out_stats) return;
    pthread_mutex_lock(&outbox->mutex);
    out_stats->enqueued = atomic_load(&outbox->enqueued);
    out_stats->dequeued = atomic_load(&outbox->dequeued);
    out_stats->coalesced = atomic_load(&outbox->coalesced);
    out_stats->rejected = atomic_load(&outbox->rejected);
    out_stats->queued_messages = outbox->queued_messages;
    out_stats->queued_bytes = outbox->queued_bytes;
    pthread_mutex_unlock(&outbox->mutex);
}
