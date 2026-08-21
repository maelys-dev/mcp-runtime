#pragma once

#include <stddef.h>

#include "maelys/mcp/error.h"

/*
 * The seam every child process this runtime starts goes through.
 * docs/launch-contract-design.md is the specification; this header is its
 * internal form. M4.1 keeps it private: nothing under include/ names a
 * launcher yet, and both consumers (src/provider/process_provider.c and
 * src/provider/mcp_proxy.c) reach it through src/internal/internal.h.
 *
 * The whole reason it exists is that a second fork/exec site is a second
 * bypass: a sandboxing policy enforcement point that governs native providers
 * but not proxied upstreams governs nothing. scripts/audit_boundaries.sh
 * enforces that no process-creation primitive lives outside src/process/, so
 * a bypass cannot merge.
 */

/*
 * Which descriptor arrangement the child gets. NOT operator configuration: it
 * is a function of the child's protocol type, derived by the runtime.
 *
 * Only STDIO exists today, and it is the zero value, so a zeroed spec
 * reproduces the arrangement both providers shipped with:
 *
 *     fd 0  <- socketpair peer
 *     fd 1  <- socketpair peer
 *     fd 2  unchanged (inherited stderr)
 *
 * The ISOLATED layout designed in docs/launch-contract-design.md ("The child's
 * descriptors") - protocol on fd 3, stdout wired to stderr - is a later wave
 * and is deliberately not declared here: an enumerator no launcher accepts
 * would advertise an arrangement that does not exist. Adding it later is
 * additive. What this wave owes it is the field, and a derivation site that
 * already fills the field in rather than assuming the default.
 */
typedef enum maelys_mcp_process_fd_layout {
    MAELYS_MCP_PROCESS_FD_STDIO = 0
} maelys_mcp_process_fd_layout_t;

/*
 * What to launch. Compiled by the runtime, never by an MCP request
 * (docs/security-model.md). Every field is read before spawn returns; a
 * launcher must copy anything it needs afterwards.
 */
typedef struct maelys_mcp_process_spec {
    /* Absolute, like every other executable path this runtime accepts. */
    const char *executable_path;
    /*
     * The COMPLETE argv, argv[0] included, NULL-terminated. Never the "extra
     * arguments" a manifest writes: a launcher that reorders, prepends or
     * interprets argv is broken.
     */
    char *const *argv;
    /*
     * Opaque to the runtime, which never parses, compares or branches on it.
     * NULL means "the launcher's own default". The POSIX launcher accepts NULL
     * and "trusted-local" and refuses everything else, loudly - a manifest
     * asking for confinement must not be answered with an unconfined process
     * and no diagnostic.
     */
    const char *execution_profile;
    /* The frame bound the runtime will enforce on this transport. Handed down
     * so a launcher can size a buffer or refuse an absurd value; the runtime
     * enforces it regardless of what the launcher does with it. */
    size_t max_message_bytes;
    /*
     * The budget spawn itself gets. A stated contract obligation, not an
     * enforced one: a fork-and-exec launcher cannot block and satisfies it
     * trivially, while a launcher talking to a remote executord can block and
     * must honour it - and the runtime, calling spawn on the startup path,
     * has no thread to time it out with. See "Budgets the runtime states but
     * cannot enforce".
     */
    unsigned int spawn_timeout_ms;
    /* The two rungs of the shutdown ladder, separately budgeted: a polite
     * request deserves longer than a kill does. */
    unsigned int grace_timeout_ms;
    unsigned int force_timeout_ms;
    /* Derived by the runtime from the child's protocol type, never configured. */
    maelys_mcp_process_fd_layout_t fd_layout;
} maelys_mcp_process_spec_t;

/* What a successful spawn produced. */
typedef struct maelys_mcp_process_instance {
    /*
     * The duplex protocol transport. Owned by the RUNTIME from the moment
     * spawn returns MAELYS_MCP_OK; the launcher must never touch it again.
     * Guaranteed by the launcher to be >= 0, blocking, and close-on-exec in
     * the RUNTIME's process, and to be an arrangement on which the runtime's
     * writes cannot raise SIGPIPE (SO_NOSIGPIPE where it exists, otherwise a
     * socket the MSG_NOSIGNAL write path can use - not a pipe).
     */
    int protocol_fd;
    /*
     * Whatever the launcher needs to identify what it started. Owned by the
     * LAUNCHER. The runtime stores it, hands it back to wait/stop/destroy, and
     * never dereferences, compares, prints or reaps it. It may be a boxed
     * pid_t, an OCI container id, a VM handle, or an executord ticket.
     *
     * NULL is a VALID handle - a stateless launcher needs no state - so handle
     * nullity must never be used as a liveness marker. That is what
     * instance_live is for.
     */
    void *handle;
    /*
     * Owned and maintained by the RUNTIME, not by the launcher, which must
     * leave it alone. Set to 1 the moment spawn returns MAELYS_MCP_OK, and
     * cleared immediately before destroy is called. It is the sole authority
     * on whether wait/stop/destroy may still be called with this handle.
     */
    int instance_live;
} maelys_mcp_process_instance_t;

typedef enum maelys_mcp_process_stop {
    /* Ask. SIGTERM on POSIX; whatever the substrate's polite request is. */
    MAELYS_MCP_PROCESS_STOP_GRACEFUL = 0,
    /* Insist. SIGKILL on POSIX. Must not block on the child. */
    MAELYS_MCP_PROCESS_STOP_FORCED = 1
} maelys_mcp_process_stop_t;

typedef struct maelys_mcp_process_ops {
    /*
     * Start it. On MAELYS_MCP_OK, *out_instance is filled and every resource
     * it names has the ownership stated above. On anything else,
     * *out_instance is untouched, nothing was left running, and the runtime
     * MUST NOT call destroy. *out_error, when set, is caller-owned.
     */
    maelys_mcp_result_t (*spawn)(
        void *context,
        const maelys_mcp_process_spec_t *spec,
        maelys_mcp_process_instance_t *out_instance,
        char **out_error);
    /*
     * Has it exited? MUST return within timeout_ms. *out_exited is 1 if the
     * launcher observed termination within the budget, 0 if it did not. Reaps
     * if the substrate needs reaping - this is the ONLY op that reaps, and the
     * only one permitted to block at all. Never called on the request path;
     * only on the shutdown ladder.
     *
     * MAELYS_MCP_OK with *out_exited == 0 after a FORCED stop is a containment
     * failure: the launcher could not terminate what it started. The ladder
     * turns it into MAELYS_MCP_ERR_STATE naming the launcher, and propagates
     * it rather than discarding it.
     */
    maelys_mcp_result_t (*wait)(
        void *context, void *handle, unsigned int timeout_ms, int *out_exited);
    /* Signal termination. MUST NOT block on the child. Idempotent: the ladder
     * calls it twice on a stubborn child, and a stop after exit is not an
     * error. */
    maelys_mcp_result_t (*stop)(
        void *context, void *handle, maelys_mcp_process_stop_t mode);
    /*
     * LOCAL RELEASE ONLY. Frees the handle and any launcher-side memory,
     * closes launcher-private descriptors, unregisters bookkeeping. It MUST
     * NOT wait for the child, MUST NOT signal it, and MUST NOT block for an
     * unbounded time - it is the one op with no timeout parameter, precisely
     * because it is not allowed to need one. A bound is worth nothing if the
     * last rung can hang.
     *
     * Called EXACTLY ONCE per spawn that returned OK, and never for one that
     * did not; the runtime guarantees this against instance_live. A handle is
     * invalid the instant this returns.
     */
    void (*destroy)(void *context, void *handle);
} maelys_mcp_process_ops_t;

typedef struct maelys_mcp_process_launcher {
    /* For diagnostics only; the runtime never branches on it. */
    const char *name;
    const maelys_mcp_process_ops_t *ops;
    void *context;
} maelys_mcp_process_launcher_t;

/* The launcher that reproduces the behaviour both providers shipped with.
 * Statically allocated, stateless, safe to share across providers and
 * threads. */
const maelys_mcp_process_launcher_t *maelys_mcp_posix_launcher(void);

/*
 * ---------------------------------------------------------------- runtime side
 * The three functions below are the runtime's half of the contract, shared by
 * every consumer so that ownership, exactly-once teardown and the bounded
 * ladder are implemented once rather than per call site. They contain no
 * process primitives themselves - only calls through the vtable.
 */

/*
 * spawn, plus the obligations the runtime owes itself: instance_live is set
 * here and nowhere else, and protocol_fd is checked for usability before any
 * caller can touch it.
 *
 * A launcher that returns MAELYS_MCP_OK with an unusable protocol_fd has
 * violated the contract in the one way only the runtime can detect. That is
 * resolved in the runtime's favour: the child is stopped and released, and the
 * launch fails with MAELYS_MCP_ERR_PROTOCOL naming the launcher.
 */
maelys_mcp_result_t maelys_mcp_process_launch(
    const maelys_mcp_process_launcher_t *launcher,
    const maelys_mcp_process_spec_t *spec,
    maelys_mcp_process_instance_t *out_instance,
    char **out_error);

/*
 * The bounded shutdown ladder, every rung budgeted:
 *
 *     stop(GRACEFUL) -> wait(grace_timeout_ms) -> stop(FORCED)
 *                    -> wait(force_timeout_ms) -> destroy
 *
 * The kind-specific goodbye - a provider/shutdown exchange for a native
 * provider, a half-close for an MCP upstream - happens ABOVE this call and
 * stays unmerged: those are protocol facts, not launch facts.
 *
 * Returns MAELYS_MCP_OK when the child was observed to exit, and
 * MAELYS_MCP_ERR_STATE with a diagnostic in *out_error when it outlived a
 * forced stop. That second case is a containment failure and a genuinely bad
 * outcome; the contract chooses the leak over the hang and requires it to be
 * reported. destroy is called either way, exactly once, and this function
 * always returns.
 *
 * A no-op returning MAELYS_MCP_OK when instance_live is already clear.
 */
maelys_mcp_result_t maelys_mcp_process_shutdown(
    const maelys_mcp_process_launcher_t *launcher,
    maelys_mcp_process_instance_t *instance,
    unsigned int grace_timeout_ms,
    unsigned int force_timeout_ms,
    char **out_error);

/*
 * The failure-path form of the ladder: stop(FORCED) -> wait -> destroy, with
 * no graceful rung. Used where a launch has already failed for a reason of the
 * runtime's own (an allocation, a thread that would not start) and the child
 * is seconds old, has produced nothing to flush, and must not be given the
 * chance to ignore a polite request - the rule PR #53 established for exactly
 * these paths. Void, because these callers already have the error they are
 * returning.
 */
void maelys_mcp_process_abandon(
    const maelys_mcp_process_launcher_t *launcher,
    maelys_mcp_process_instance_t *instance,
    unsigned int force_timeout_ms);
