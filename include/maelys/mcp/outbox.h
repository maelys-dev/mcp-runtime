#pragma once

#include <stddef.h>
#include <jansson.h>

#include "maelys/mcp/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_mcp_outbox maelys_mcp_outbox_t;

typedef enum maelys_mcp_outbox_class {
    MAELYS_MCP_OUTBOX_RESPONSE = 0,
    MAELYS_MCP_OUTBOX_NOTIFICATION = 1
} maelys_mcp_outbox_class_t;

typedef struct maelys_mcp_outbox_config {
    size_t max_messages;
    size_t max_bytes;
    size_t response_burst;
    unsigned int admission_timeout_ms;
} maelys_mcp_outbox_config_t;

typedef struct maelys_mcp_outbox_stats {
    unsigned long long enqueued;
    unsigned long long dequeued;
    unsigned long long coalesced;
    unsigned long long rejected;
    size_t queued_messages;
    size_t queued_bytes;
} maelys_mcp_outbox_stats_t;

/* Creates a passive bounded queue. No thread is created. */
maelys_mcp_result_t maelys_mcp_outbox_create(
    const maelys_mcp_outbox_config_t *config,
    maelys_mcp_outbox_t **out_outbox);

/*
 * On success the outbox steals one reference to message. On failure the caller
 * retains ownership. A non-empty coalesce_key is valid only for notifications;
 * replacing an existing notification moves it to the newest causal position.
 */
maelys_mcp_result_t maelys_mcp_outbox_enqueue_take(
    maelys_mcp_outbox_t *outbox,
    json_t *message,
    maelys_mcp_outbox_class_t message_class,
    const char *coalesce_key);

/* Returns one owned message, timeout, or CLOSED after the queue is drained. */
maelys_mcp_result_t maelys_mcp_outbox_next(
    maelys_mcp_outbox_t *outbox,
    unsigned int timeout_ms,
    json_t **out_message);

/* Stops admission and wakes all producers/consumers. */
maelys_mcp_result_t maelys_mcp_outbox_close(
    maelys_mcp_outbox_t *outbox,
    int discard);

/* Waits until a closed queue is drained, or until the monotonic timeout. */
maelys_mcp_result_t maelys_mcp_outbox_wait_drained(
    maelys_mcp_outbox_t *outbox,
    unsigned int timeout_ms);

/* Frees a closed and drained queue after awakened waiters have returned. */
maelys_mcp_result_t maelys_mcp_outbox_destroy(
    maelys_mcp_outbox_t *outbox);

void maelys_mcp_outbox_stats(
    maelys_mcp_outbox_t *outbox,
    maelys_mcp_outbox_stats_t *out_stats);

#ifdef __cplusplus
}
#endif
