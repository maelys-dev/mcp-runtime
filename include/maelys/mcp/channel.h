#pragma once

#include <stddef.h>
#include <jansson.h>

#include "maelys/mcp/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_mcp_runtime maelys_mcp_runtime_t;
typedef struct maelys_mcp_channel maelys_mcp_channel_t;

typedef struct maelys_mcp_channel_config {
    size_t max_messages;
    size_t max_bytes;
    size_t response_burst;
    unsigned int admission_timeout_ms;
    unsigned int close_timeout_ms;
} maelys_mcp_channel_config_t;

/*
 * Creates and publishes a channel owned by runtime. Provider activation occurs
 * once, on the first channel creation. A published channel keeps runtime alive
 * until maelys_mcp_channel_destroy succeeds.
 */
maelys_mcp_result_t maelys_mcp_channel_create(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_channel_config_t *config,
    maelys_mcp_channel_t **out_channel);

/* Dispatches request and admits every response through this channel's outbox. */
maelys_mcp_result_t maelys_mcp_channel_handle(
    maelys_mcp_channel_t *channel,
    json_t *request);

/*
 * Returns one owned message, MAELYS_MCP_ERR_TIMEOUT when the monotonic wait
 * expires, or MAELYS_MCP_ERR_CLOSED once a closed channel has been drained.
 */
maelys_mcp_result_t maelys_mcp_channel_next(
    maelys_mcp_channel_t *channel,
    unsigned int timeout_ms,
    json_t **out_message);

/*
 * Stops new work and fan-out, completes active subscriptions, and drains
 * admitted output within one monotonic timeout_ms deadline shared by every
 * close phase. Zero selects the configured timeout.
 */
maelys_mcp_result_t maelys_mcp_channel_close(
    maelys_mcp_channel_t *channel,
    unsigned int timeout_ms);

/*
 * Closes when needed, unregisters the channel, and frees it. The handle is
 * consumed even if the bounded close reports an error, so it must never be
 * used or destroyed again after this call returns.
 */
maelys_mcp_result_t maelys_mcp_channel_destroy(
    maelys_mcp_channel_t *channel);

#ifdef __cplusplus
}
#endif
