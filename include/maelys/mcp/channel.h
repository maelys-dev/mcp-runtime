#pragma once

#include <stddef.h>
#include <jansson.h>

#include "maelys/mcp/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_mcp_runtime maelys_mcp_runtime_t;
typedef struct maelys_mcp_channel maelys_mcp_channel_t;

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
     * locking behind the pointer. The embedder must map one channel to one
     * principal, which is its obligation and is not checked by the runtime.
     *
     * Who frees what this designates depends on context_release below: with
     * no callback the embedder owns it and must not free it until
     * maelys_mcp_channel_destroy has returned; with one, the runtime hands it
     * back exactly once at the real free, which is the only form that works
     * for maelys_mcp_channel_destroy_detached.
     */
    void *context;
    /*
     * Which protocol eras this channel serves and announces, for this
     * channel's whole life: a non-empty subset of MAELYS_MCP_ERA_ALL, or zero
     * for MAELYS_MCP_ERA_ALL. Zero is the default rather than an error
     * because a zero-initialized config is what every caller that never had
     * an opinion about eras already passes, and it must keep meaning "both".
     * A non-zero mask carrying any bit outside MAELYS_MCP_ERA_ALL is
     * MAELYS_MCP_ERR_ARGUMENT from maelys_mcp_channel_create: silently
     * reinterpreting an impossible mask as "all of them" would hide the
     * caller's mistake.
     *
     * Three things change when an era is left out:
     *
     *   - `server/discover` announces only the eras set in
     *     `result.supportedVersions`;
     *   - `initialize` is refused with -32600 when MAELYS_MCP_ERA_LEGACY is
     *     absent;
     *   - a request carrying modern `_meta` negotiation is refused with
     *     -32022 when MAELYS_MCP_ERA_MODERN is absent, and the error's
     *     `data.supported` lists what this channel does serve.
     *
     * Fixed at creation on purpose. The 0.17.0 setter this replaced had to
     * refuse a closing channel, and had to refuse withdrawing
     * MAELYS_MCP_ERA_LEGACY from a channel that had already accepted an
     * `initialize` - because a negotiated era cannot be taken back from a
     * client already using it. A value that is decided before the channel
     * exists cannot be taken back from anyone, so neither rule survives.
     */
    unsigned int protocol_eras;
    /*
     * Called exactly once with (release_context, context) at the moment
     * `context` stops being reachable, and never called at all when NULL -
     * which is today's behaviour, in which the runtime owns nothing.
     *
     * "Exactly once, whichever path destruction took": the synchronous
     * maelys_mcp_channel_destroy calls it before returning, and
     * maelys_mcp_channel_destroy_detached calls it from whichever in-flight
     * operation finishes last, which may be long after that call returned and
     * is on a thread the embedder never created. It is therefore the only
     * signal that the context can be freed, and an embedder must not also
     * free on the return from destroy.
     *
     * The callback runs after every other part of the channel is gone and
     * with no runtime or channel lock held, so it may do anything except
     * touch the channel it was bound to - that handle is already consumed.
     * It runs before maelys_mcp_runtime_destroy returns, because that call
     * waits for every detached channel.
     *
     * Ownership transfers only on success: a maelys_mcp_channel_create that
     * returns anything but MAELYS_MCP_OK never took the context and never
     * calls this.
     */
    void (*context_release)(void *release_context, void *context);
    void *release_context;
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

/*
 * As maelys_mcp_channel_destroy, except that it never waits without a bound.
 *
 * maelys_mcp_channel_destroy answers a bounded close that missed its deadline
 * by waiting for the channel to drain however long that takes, because the
 * alternative - freeing at the deadline, while workers still hold the
 * channel's mutex and outbox - is a use-after-free. That is the right trade
 * for a transport that destroys one channel at process exit. It is the wrong
 * trade for a transport that destroys a channel per request, where one wedged
 * in-process provider would park the calling thread, and whatever the caller
 * was holding for it, for as long as the provider takes.
 *
 * This entry point makes the same guarantee without the wait. When the
 * bounded close misses its deadline the channel is aborted, taken out of the
 * runtime immediately, and marked detached; this call then returns
 * MAELYS_MCP_ERR_TIMEOUT, and whichever in-flight operation finishes last
 * performs the real free on its own thread. The handle is consumed either
 * way, exactly as with maelys_mcp_channel_destroy, and must never be used or
 * destroyed again.
 *
 * The channel's context (maelys_mcp_channel_config_t::context) may therefore
 * outlive this call, and this is the entry point
 * maelys_mcp_channel_config_t::context_release exists for: whichever thread
 * performs the real free calls it, so an embedder learns when its context
 * stops being reachable instead of having to wait for
 * maelys_mcp_runtime_destroy. An embedder that sets no callback still owns
 * the context and still has that wait as its only bound.
 *
 * MAELYS_MCP_OK means the channel closed cleanly and was freed before this
 * returned; there is nothing outstanding. Any other status is the bounded
 * close's, and the free may still be pending.
 */
maelys_mcp_result_t maelys_mcp_channel_destroy_detached(
    maelys_mcp_channel_t *channel);

#ifdef __cplusplus
}
#endif
