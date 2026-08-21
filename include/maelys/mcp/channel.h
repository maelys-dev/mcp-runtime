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
    /*
     * Opaque, embedder-owned. The runtime carries this pointer for the
     * channel's lifetime, never dereferences it, and never copies what it
     * designates - it is the same pointer for the whole lifetime, read-only
     * from the runtime's side. An adapter needing mutability owns its own
     * locking behind the pointer. The embedder must not free what it
     * designates until maelys_mcp_channel_destroy has returned, and must map
     * one channel to one principal; both are the embedder's obligation and
     * are not checked by the runtime.
     */
    void *context;
} maelys_mcp_channel_config_t;

/*
 * The protocol eras a channel serves and announces. Version policy is a
 * property of the channel, not of the transport and not of the runtime: one
 * runtime can serve a stdio channel that speaks both eras and a channel that
 * speaks only 2026-07-28 at the same time, and no transport ever rewrites a
 * dispatch result to make that true.
 */
typedef enum maelys_mcp_protocol_era {
    /* 2024-11-05 ... 2025-11-25, negotiated by the `initialize` handshake. */
    MAELYS_MCP_ERA_LEGACY = 1u << 0,
    /* 2026-07-28, negotiated per request through `_meta`. */
    MAELYS_MCP_ERA_MODERN = 1u << 1
} maelys_mcp_protocol_era_t;

#define MAELYS_MCP_ERA_ALL \
    ((unsigned int)MAELYS_MCP_ERA_LEGACY | (unsigned int)MAELYS_MCP_ERA_MODERN)

/*
 * Creates and publishes a channel owned by runtime. Provider activation occurs
 * once, on the first channel creation. A published channel keeps runtime alive
 * until maelys_mcp_channel_destroy succeeds.
 */
maelys_mcp_result_t maelys_mcp_channel_create(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_channel_config_t *config,
    maelys_mcp_channel_t **out_channel);

/*
 * Restricts the eras this channel serves. `eras` is a non-empty subset of
 * MAELYS_MCP_ERA_ALL; anything else is MAELYS_MCP_ERR_ARGUMENT, because a
 * channel that serves no era can answer nothing and silently reinterpreting
 * that as "all of them" would hide the caller's mistake.
 *
 * Every channel starts at MAELYS_MCP_ERA_ALL, so a caller that never makes
 * this call observes exactly the behaviour it always did. Three things change
 * when an era is cleared:
 *
 *   - `server/discover` announces only the eras still set in
 *     `result.supportedVersions`;
 *   - `initialize` is refused with -32600 once MAELYS_MCP_ERA_LEGACY is clear;
 *   - a request carrying modern `_meta` negotiation is refused with -32022
 *     once MAELYS_MCP_ERA_MODERN is clear, and the error's `data.supported`
 *     lists what this channel does serve.
 *
 * Callable on an active channel, normally immediately after creation and
 * before any traffic. Returns MAELYS_MCP_ERR_STATE for a channel that is
 * closing or faulted, and for an attempt to withdraw MAELYS_MCP_ERA_LEGACY
 * from a channel that has already accepted an `initialize`: a negotiated era
 * cannot be taken back from a client that is already using it.
 *
 * This is a setter rather than a `maelys_mcp_channel_config_t` field or a new
 * constructor on purpose. Widening that released public structure would be an
 * ABI break (docs/abi-policy.md), and which permanent public shape the
 * capability should eventually take - a field behind an ABI 4 bump, or a
 * size-prefixed options struct behind a `_ex` constructor - is an open
 * question for the repository owner. A new entry point is the additive idiom
 * docs/abi-policy.md names as this project's preferred one, keeps
 * MAELYS_MCP_ABI_VERSION at 3, and leaves that choice open rather than
 * pre-empting it.
 */
maelys_mcp_result_t maelys_mcp_channel_set_protocol_eras(
    maelys_mcp_channel_t *channel,
    unsigned int eras);

/* Dispatches request and admits every response through this channel's outbox. */
maelys_mcp_result_t maelys_mcp_channel_handle(
    maelys_mcp_channel_t *channel,
    json_t *request);

/*
 * Returns the opaque context bound at channel creation (config->context), or
 * NULL if none was bound. Returns NULL for a NULL channel.
 */
void *maelys_mcp_channel_context(const maelys_mcp_channel_t *channel);

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
