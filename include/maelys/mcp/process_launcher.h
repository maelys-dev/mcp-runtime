#pragma once

#include <stddef.h>

#include "maelys/mcp/error.h"
#include "maelys/mcp/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The seam every child process this runtime starts goes through: native
 * maelys-provider children and mcp-proxy upstreams alike.
 *
 * It exists so that confinement - a sandbox, a container, a remote executord -
 * can be installed once and govern every launch. A second fork/exec site would
 * be a second bypass, so scripts/audit_boundaries.sh forbids process-creation
 * primitives outside src/process/, and both provider kinds reach the substrate
 * only through the four ops below. Nothing in this runtime knows what a
 * sandbox is; a launcher does.
 *
 * The full contract - ownership, the bounded termination ladder, and the parts
 * of it the runtime states but cannot enforce - is docs/launch-contract-design.md.
 *
 * ABI note: these are new structures reached by new entry points, which is
 * this project's idiom for evolving the API without breaking a released layout
 * (docs/abi-policy.md). Nothing already released changes shape, so
 * MAELYS_MCP_ABI_VERSION stays where it is, and there is deliberately no
 * struct_size field anywhere here: this project wants a checkable ABI version
 * and a link error, not silent tolerance of mismatched builds.
 */

/*
 * Which descriptor arrangement the child gets. NOT operator configuration: it
 * is a function of the child's protocol type, derived by the runtime.
 *
 * STDIO is the zero value, so a zeroed spec reproduces the arrangement both
 * providers shipped with:
 *
 *     fd 0  <- socketpair peer
 *     fd 1  <- socketpair peer
 *     fd 2  unchanged (inherited stderr)
 *
 * ISOLATED (docs/launch-contract-design.md, "The child's descriptors") is:
 *
 *     fd 0  <- /dev/null                nothing to read from stdin
 *     fd 1  <- dup of fd 2              stdout IS stderr, at the kernel level
 *     fd 2  unchanged (inherited stderr)
 *     fd 3  <- socketpair peer          duplex protocol transport
 *     env   MAELYS_PROVIDER_FD=3
 *
 * Available only to native maelys-provider children, whose first-party SDKs
 * can be taught MAELYS_PROVIDER_FD; an mcp-proxy upstream speaks MCP's stdio
 * transport by specification and stays STDIO structurally -
 * maelys_mcp_proxy_options_t carries no layout field, so this kind cannot be
 * given ISOLATED even by mistake. Selection this release is via the public
 * spec only: neither the stock host nor the manifest chooses it yet - STDIO
 * stays the native default through M4, the first step of the transition plan
 * ("The layout is a function of the child's protocol type").
 */
typedef enum maelys_mcp_process_fd_layout {
    MAELYS_MCP_PROCESS_FD_STDIO = 0,
    MAELYS_MCP_PROCESS_FD_ISOLATED = 1
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
 * The provider entry points that take a launcher. maelys_mcp_provider_spawn,
 * maelys_mcp_provider_spawn_with_options and maelys_mcp_provider_proxy_spawn
 * are unchanged and behave exactly as before; each is now a thin wrapper that
 * binds maelys_mcp_posix_launcher().
 *
 * `launcher` must be non-NULL, and must outlive the provider: one launcher is
 * bound per provider at spawn and lives for that provider's lifetime. Passing
 * NULL is MAELYS_MCP_ERR_ARGUMENT rather than a silent fall back to the POSIX
 * launcher - the point of these entry points is to name the launcher, so a
 * caller who names nothing has made a mistake worth hearing about.
 */
maelys_mcp_result_t maelys_mcp_provider_spawn_with_launcher(
    const maelys_mcp_provider_process_options_t *options,
    const maelys_mcp_process_launcher_t *launcher,
    maelys_mcp_provider_t **out_provider,
    char **out_error);

maelys_mcp_result_t maelys_mcp_provider_proxy_spawn_with_launcher(
    const maelys_mcp_proxy_options_t *options,
    const maelys_mcp_process_launcher_t *launcher,
    maelys_mcp_provider_t **out_provider,
    char **out_skipped_tools,
    char **out_error);

#ifdef __cplusplus
}
#endif
