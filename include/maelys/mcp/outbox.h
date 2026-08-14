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

typedef maelys_mcp_result_t (*maelys_mcp_outbox_write_fn)(
    void *context,
    json_t *message);

typedef struct maelys_mcp_outbox_config {
    size_t max_messages;
    size_t max_bytes;
    size_t batch_size;
    size_t response_burst;
    maelys_mcp_outbox_write_fn write;
    void *write_context;
} maelys_mcp_outbox_config_t;

typedef struct maelys_mcp_outbox_stats {
    unsigned long long enqueued;
    unsigned long long written;
    unsigned long long coalesced;
    size_t queued_messages;
    size_t queued_bytes;
} maelys_mcp_outbox_stats_t;

/*
 * Starts the unique writer thread. The write callback is never called while the
 * queue mutex is held.
 */
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

/* Stops accepting messages, drains when requested, joins the writer and frees it. */
maelys_mcp_result_t maelys_mcp_outbox_destroy(
    maelys_mcp_outbox_t *outbox,
    int drain);

void maelys_mcp_outbox_stats(
    maelys_mcp_outbox_t *outbox,
    maelys_mcp_outbox_stats_t *out_stats);

#ifdef __cplusplus
}
#endif
