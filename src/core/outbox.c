#include "src/internal/internal.h"
#include "maelys/mcp/outbox.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    pthread_cond_t idle;
    maelys_mcp_outbox_config_t config;
    outbox_queue_t responses;
    outbox_queue_t notifications;
    size_t queued_messages;
    size_t queued_bytes;
    size_t consecutive_responses;
    size_t waiters;
    int accepting;
    /*
     * The lazy readiness pipe. Both descriptors are -1 until a transport asks
     * for one with maelys_mcp_outbox_enable_wait_fd, which is what keeps stdio
     * at two descriptors per channel instead of four and at zero extra writes.
     *
     * `wakeup_raised` is the flag, not the pipe: it records whether a byte has
     * been written (or is on its way) for the current state, so a burst of
     * enqueues costs one write(2) rather than one per message. It is
     * maintained under this mutex, next to queued_messages and accepting,
     * which is what makes "readable exactly when next() would not block" a
     * property of one critical section instead of an agreement between two.
     */
    int wait_read_fd;
    int wait_write_fd;
    int wakeup_raised;
    atomic_ullong enqueued;
    atomic_ullong dequeued;
    atomic_ullong coalesced;
    atomic_ullong rejected;
};

/*
 * What the readiness descriptor owes the queue's current state, decided under
 * the mutex and performed after it is released.
 *
 * The syscall is deliberately not issued here. The pipe's write end is
 * non-blocking, so it could not park a producer, but keeping any descriptor
 * work out of the critical section is what lets this seam claim to add no
 * lock and no lock-ordering edge: everything it touches is already the
 * innermost lock on the enqueue path.
 *
 * Splitting the decision from the syscall is safe because the flag is what
 * arbitrates, not the pipe. Each raise schedules exactly one write of one
 * byte and each lower schedules exactly one read of one byte, so at any
 * quiescent moment the bytes in the pipe are (writes - reads), which is at
 * least `wakeup_raised`: a read that overtakes its write finds the pipe empty
 * and returns EAGAIN, leaving the count higher rather than lower. The
 * descriptor can therefore be spuriously readable - the caller answers that
 * with a next() that reports ERR_TIMEOUT - and can never be silently
 * unreadable while the queue has something in it, which is the failure that
 * would wedge a poller.
 */
typedef enum wakeup_action {
    WAKEUP_NONE = 0,
    WAKEUP_RAISE = 1,
    WAKEUP_LOWER = 2
} wakeup_action_t;

static wakeup_action_t wakeup_action_locked(
    maelys_mcp_outbox_t *outbox,
    int *out_fd) {
    *out_fd = -1;
    if (outbox->wait_read_fd < 0) return WAKEUP_NONE;
    /*
     * Readable exactly when maelys_mcp_outbox_next would answer immediately:
     * a queued message, or a closed outbox whose ERR_CLOSED the poller still
     * has to collect. The second half is why a closed-and-drained outbox stays
     * readable - a poller that was told "nothing here" and never told "and
     * there never will be" would wait for its whole timeout to learn that the
     * conversation is over.
     */
    int readable = outbox->queued_messages != 0u || !outbox->accepting;
    if (readable == outbox->wakeup_raised) return WAKEUP_NONE;
    outbox->wakeup_raised = readable;
    *out_fd = readable ? outbox->wait_write_fd : outbox->wait_read_fd;
    return readable ? WAKEUP_RAISE : WAKEUP_LOWER;
}

static void apply_wakeup(wakeup_action_t action, int fd) {
    unsigned char byte = 1u;
    if (action == WAKEUP_RAISE) {
        ssize_t written = write(fd, &byte, sizeof(byte));
        (void)written;
    } else if (action == WAKEUP_LOWER) {
        /* Exactly one byte, never "until EAGAIN": a drain that swallowed a
         * byte belonging to a raise issued after this decision would leave the
         * queue non-empty and the descriptor unreadable, which is the one
         * failure mode this seam must not have. */
        ssize_t consumed = read(fd, &byte, sizeof(byte));
        (void)consumed;
    }
}

static int wait_until(
    maelys_mcp_outbox_t *outbox,
    pthread_cond_t *condition,
    uint64_t deadline_ms) {
    outbox->waiters++;
    int status = maelys_mcp_cond_wait_until(condition, &outbox->mutex, deadline_ms);
    outbox->waiters--;
    if (outbox->waiters == 0u) pthread_cond_broadcast(&outbox->idle);
    return status;
}

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
    /* Not calloc's zero: zero is a real descriptor. */
    outbox->wait_read_fd = -1;
    outbox->wait_write_fd = -1;
    if (pthread_mutex_init(&outbox->mutex, NULL) != 0) goto failed;
    if (maelys_mcp_cond_init_monotonic(&outbox->available) != 0) goto failed_mutex;
    if (maelys_mcp_cond_init_monotonic(&outbox->space) != 0) goto failed_available;
    if (maelys_mcp_cond_init_monotonic(&outbox->idle) != 0) goto failed_space;
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
    uint64_t deadline_ms = 0u;
    if (!outbox || maelys_mcp_monotonic_deadline(
            outbox->config.admission_timeout_ms, &deadline_ms) != 0) {
        return outbox ? MAELYS_MCP_ERR_IO : MAELYS_MCP_ERR_ARGUMENT;
    }
    return maelys_mcp_outbox_enqueue_take_until(
        outbox, message, message_class, coalesce_key, deadline_ms);
}

maelys_mcp_result_t maelys_mcp_outbox_enqueue_take_until(
    maelys_mcp_outbox_t *outbox,
    json_t *message,
    maelys_mcp_outbox_class_t message_class,
    const char *coalesce_key,
    uint64_t deadline_ms) {
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
    pthread_mutex_lock(&outbox->mutex);
    for (;;) {
        if (!outbox->accepting) {
            atomic_fetch_add(&outbox->rejected, 1u);
            pthread_mutex_unlock(&outbox->mutex);
            free(key);
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
                int wakeup_fd = -1;
                wakeup_action_t wakeup = wakeup_action_locked(outbox, &wakeup_fd);
                pthread_cond_signal(&outbox->available);
                pthread_mutex_unlock(&outbox->mutex);
                apply_wakeup(wakeup, wakeup_fd);
                json_decref(old);
                free(key);
                return MAELYS_MCP_OK;
            }
        } else if (outbox->queued_messages < outbox->config.max_messages &&
                   outbox->queued_bytes + bytes <= outbox->config.max_bytes) {
            outbox_node_t *created = calloc(1u, sizeof(*created));
            if (!created) {
                atomic_fetch_add(&outbox->rejected, 1u);
                pthread_mutex_unlock(&outbox->mutex);
                free(key);
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
            int wakeup_fd = -1;
            wakeup_action_t wakeup = wakeup_action_locked(outbox, &wakeup_fd);
            pthread_cond_signal(&outbox->available);
            pthread_mutex_unlock(&outbox->mutex);
            apply_wakeup(wakeup, wakeup_fd);
            return MAELYS_MCP_OK;
        }
        int waited = wait_until(outbox, &outbox->space, deadline_ms);
        if (waited == ETIMEDOUT) {
            atomic_fetch_add(&outbox->rejected, 1u);
            pthread_mutex_unlock(&outbox->mutex);
            free(key);
            return MAELYS_MCP_ERR_TIMEOUT;
        }
        if (waited != 0) {
            atomic_fetch_add(&outbox->rejected, 1u);
            pthread_mutex_unlock(&outbox->mutex);
            free(key);
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
        int waited = wait_until(outbox, &outbox->available, deadline_ms);
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
    int wakeup_fd = -1;
    wakeup_action_t wakeup = wakeup_action_locked(outbox, &wakeup_fd);
    pthread_cond_broadcast(&outbox->space);
    pthread_mutex_unlock(&outbox->mutex);
    apply_wakeup(wakeup, wakeup_fd);
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
    /* Raises whatever the queue looks like, because a closed outbox is
     * something a poller has to be told about: the answer it will now get is
     * ERR_CLOSED, and it must not wait out its timeout to hear it. */
    int wakeup_fd = -1;
    wakeup_action_t wakeup = wakeup_action_locked(outbox, &wakeup_fd);
    pthread_cond_broadcast(&outbox->available);
    pthread_cond_broadcast(&outbox->space);
    pthread_mutex_unlock(&outbox->mutex);
    apply_wakeup(wakeup, wakeup_fd);
    return MAELYS_MCP_OK;
}

maelys_mcp_result_t maelys_mcp_outbox_enable_wait_fd(
    maelys_mcp_outbox_t *outbox) {
    if (!outbox) return MAELYS_MCP_ERR_ARGUMENT;
    int descriptors[2] = { -1, -1 };
    maelys_mcp_result_t status = maelys_mcp_create_wakeup_pipe(descriptors);
    if (status != MAELYS_MCP_OK) return status;
    int wakeup_fd = -1;
    wakeup_action_t wakeup = WAKEUP_NONE;
    pthread_mutex_lock(&outbox->mutex);
    int already_enabled = outbox->wait_read_fd >= 0;
    if (!already_enabled) {
        outbox->wait_read_fd = descriptors[0];
        outbox->wait_write_fd = descriptors[1];
        /*
         * Synced immediately, because enabling is not necessarily the first
         * thing that happens to an outbox: one that already holds a message,
         * or that is already closed, owes its new poller a wakeup it would
         * otherwise only get on the next state change that never comes.
         */
        wakeup = wakeup_action_locked(outbox, &wakeup_fd);
    }
    pthread_mutex_unlock(&outbox->mutex);
    if (already_enabled) {
        /* Idempotent, and the first pipe is the one that stands: a caller
         * that already has the descriptor must keep getting that one. */
        close(descriptors[0]);
        close(descriptors[1]);
        return MAELYS_MCP_OK;
    }
    apply_wakeup(wakeup, wakeup_fd);
    return MAELYS_MCP_OK;
}

int maelys_mcp_outbox_wait_fd(maelys_mcp_outbox_t *outbox) {
    if (!outbox) return -1;
    pthread_mutex_lock(&outbox->mutex);
    int fd = outbox->wait_read_fd;
    pthread_mutex_unlock(&outbox->mutex);
    return fd;
}

maelys_mcp_result_t maelys_mcp_outbox_wait_drained(
    maelys_mcp_outbox_t *outbox,
    unsigned int timeout_ms) {
    if (!outbox || !timeout_ms) return MAELYS_MCP_ERR_ARGUMENT;
    uint64_t deadline_ms = 0u;
    if (maelys_mcp_monotonic_deadline(timeout_ms, &deadline_ms) != 0) {
        return MAELYS_MCP_ERR_IO;
    }
    return maelys_mcp_outbox_wait_drained_until(outbox, deadline_ms);
}

maelys_mcp_result_t maelys_mcp_outbox_wait_drained_until(
    maelys_mcp_outbox_t *outbox,
    uint64_t deadline_ms) {
    if (!outbox) return MAELYS_MCP_ERR_ARGUMENT;
    pthread_mutex_lock(&outbox->mutex);
    if (outbox->accepting) {
        pthread_mutex_unlock(&outbox->mutex);
        return MAELYS_MCP_ERR_STATE;
    }
    while (outbox->queued_messages != 0u) {
        int waited = wait_until(outbox, &outbox->space, deadline_ms);
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
    while (outbox->waiters != 0u) {
        pthread_cond_wait(&outbox->idle, &outbox->mutex);
    }
    int read_fd = outbox->wait_read_fd;
    int write_fd = outbox->wait_write_fd;
    outbox->wait_read_fd = -1;
    outbox->wait_write_fd = -1;
    pthread_mutex_unlock(&outbox->mutex);
    /*
     * Under the same rule as the mutex and conditions below: an outbox is
     * destroyed only once nothing is inside it, so no thread can still be in
     * apply_wakeup with these numbers when they stop being this outbox's.
     * Cleared under the lock first, so a caller that asks for the descriptor
     * afterwards is told -1 rather than a number that is about to be reused.
     */
    if (read_fd >= 0) close(read_fd);
    if (write_fd >= 0) close(write_fd);
    pthread_cond_destroy(&outbox->idle);
    pthread_cond_destroy(&outbox->space);
    pthread_cond_destroy(&outbox->available);
    pthread_mutex_destroy(&outbox->mutex);
    free(outbox);
    return MAELYS_MCP_OK;
}

size_t maelys_mcp_outbox_waiter_count(maelys_mcp_outbox_t *outbox) {
    if (!outbox) return 0u;
    pthread_mutex_lock(&outbox->mutex);
    size_t count = outbox->waiters;
    pthread_mutex_unlock(&outbox->mutex);
    return count;
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
