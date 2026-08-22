#pragma once

#include <stdatomic.h>

#include "maelys/mcp/process_launcher.h"

/*
 * The runtime's half of the ABI 5 launch contract. The seam's types, the four
 * ops and the POSIX launcher are public (maelys/mcp/process_launcher.h); what
 * is below is not, because it is how *this* runtime holds up its end -
 * ownership, exactly-once teardown, the bounded ladder, and the two structures
 * the public header deliberately made opaque.
 *
 * None of it contains a process primitive; it only calls through the vtable,
 * which is what lets a container or an executord launcher fit without a line
 * changing here.
 */

/*
 * The launcher, opaque above this file. Refcounted because ABI 4's borrowed
 * pointer carried a written obligation ("must outlive the provider") and no
 * mechanism to keep it; `references` is atomic because the last reference is
 * dropped by whichever provider teardown finishes last, which is not in
 * general the thread that created the launcher.
 *
 * `ops` is BORROWED and never freed - that is what lets an ops table live in
 * .rodata and be shared by every launcher an embedder creates. `name` is
 * copied, because it has to stay readable after the caller's string is gone.
 */
struct maelys_mcp_process_launcher {
    char *name;
    const maelys_mcp_process_ops_t *ops;
    void *context;
    void (*release_context)(void *context);
    atomic_uint references;
};

/*
 * The child environment, compiled ONCE here rather than privately by each
 * launcher. Under ABI 4 every launcher rebuilt the same closed allowlist for
 * itself, which is two implementations of one rule the day there are two
 * launchers, and the one that drifts is the one nobody tested.
 *
 * PATH is platform-specific in exactly the way the ABI 4 POSIX launcher's own
 * allowlist was: Homebrew's prefix is where a macOS developer's interpreters
 * actually live, and putting it on a Linux child's PATH would name a directory
 * that does not exist. It is a fact about the machine the child runs on, which
 * is why "The environment platform rule" forbids carrying it to a target that
 * is not this one.
 */
#ifdef __APPLE__
#define MAELYS_MCP_PROCESS_CHILD_PATH \
    "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin"
#else
#define MAELYS_MCP_PROCESS_CHILD_PATH "/usr/local/bin:/usr/bin:/bin"
#endif

/*
 * Where the protocol lands under MAELYS_MCP_PROCESS_FD_ISOLATED, and the text
 * MAELYS_PROVIDER_FD carries to the child so its SDK knows where to look.
 *
 * The two must agree, and under ABI 5 they are agreed on across the seam
 * rather than within one file: the RUNTIME writes the environment variable
 * when it compiles the request, and the LAUNCHER chooses the descriptor. One
 * definition, used by both halves, is what keeps that from becoming two
 * numbers that happen to match today.
 */
#define MAELYS_MCP_PROCESS_ISOLATED_FD 3
#define MAELYS_MCP_PROCESS_ISOLATED_FD_TEXT "3"

/* PATH, LANG, LC_ALL, and MAELYS_PROVIDER_FD under ISOLATED. A closed set, so
 * the request carries it inline and a launch allocates nothing for it. */
#define MAELYS_MCP_PROCESS_MAX_ENVIRONMENT 4u

/*
 * The opaque request, defined here and nowhere else.
 *
 * It is built on the stack of maelys_mcp_process_launch and dies with that
 * frame, which is how the header's one load-bearing rule - every pointer a
 * getter returns is valid only for the duration of the spawn call in which it
 * was obtained - is enforced by scope rather than by discipline. Nothing in
 * the runtime can hand a launcher a request that outlives its spawn, because
 * there is no request that outlives its spawn.
 *
 * Every string in it is borrowed: from the caller's options, from the argv the
 * caller compiled, or from a string literal. The struct owns nothing and needs
 * no teardown.
 */
struct maelys_mcp_process_request {
    const char *executable;
    /* The COMPLETE vector, argv[0] included, NULL-terminated because that is
     * the shape both call sites already hold one in. `arg_count` is what the
     * getters expose; the terminator is not, because the contract requires a
     * launcher to build its own execve-shaped vector. */
    char *const *argv;
    size_t arg_count;
    const char *execution_profile;
    maelys_mcp_process_fd_layout_t fd_layout;
    unsigned int spawn_timeout_ms;
    unsigned int grace_timeout_ms;
    unsigned int force_timeout_ms;
    const char *environment_platform;
    const char *environment_names[MAELYS_MCP_PROCESS_MAX_ENVIRONMENT];
    const char *environment_values[MAELYS_MCP_PROCESS_MAX_ENVIRONMENT];
    size_t environment_count;
};

/*
 * What a caller of maelys_mcp_process_launch supplies: the launch facts that
 * are the caller's to decide. The rest of the request - the environment, the
 * platform token - is the runtime's own decision and is compiled by
 * maelys_mcp_process_launch, so no call site can get it individually wrong.
 *
 * This is ABI 4's maelys_mcp_process_spec_t minus everything that was only
 * ever the runtime's: it is internal now, so extending it is a recompile
 * rather than an ABI event. max_message_bytes is not here, matching "What ABI
 * 5 deliberately does not carry" - the runtime enforces the bound itself and
 * no launcher ever read it.
 */
typedef struct maelys_mcp_process_launch_params {
    const char *executable_path;
    char *const *argv;
    const char *execution_profile;
    maelys_mcp_process_fd_layout_t fd_layout;
    unsigned int spawn_timeout_ms;
    unsigned int grace_timeout_ms;
    unsigned int force_timeout_ms;
} maelys_mcp_process_launch_params_t;

/*
 * The runtime's private record of one launch: ABI 4's
 * maelys_mcp_process_instance_t, moved out of the launcher's reach.
 *
 * `live` is the exactly-once flag the public header says is absent from it,
 * and this is where it went. Under ABI 4 it lived in a structure the launcher
 * was handed and could write, which made the exactly-once guarantee for
 * release rest on a comment; here no launcher can reach it.
 *
 * `launcher` is the PROVIDER'S OWN reference, taken by
 * maelys_mcp_process_launch on the success path and released by whichever of
 * the two terminal operations runs. The caller's reference is independent of
 * it, which is the whole of the lifetime rule ABI 5 replaced a sentence with.
 */
typedef struct maelys_mcp_process_slot {
    maelys_mcp_process_launcher_t *launcher;
    void *handle;
    int protocol_fd;
    int live;
} maelys_mcp_process_slot_t;

/*
 * spawn, plus the obligations the runtime owes itself: the request is compiled
 * and destroyed here, `live` is set here and nowhere else, the launcher is
 * retained for the provider, and protocol_fd is checked for usability before
 * any caller can touch it.
 *
 * A launcher that returns MAELYS_MCP_OK with an unusable protocol_fd has
 * violated the contract in the one way only the runtime can detect. That is
 * resolved in the runtime's favour: the child is stopped and released, the
 * reference just taken is given back, and the launch fails with
 * MAELYS_MCP_ERR_PROTOCOL naming the launcher.
 *
 * On MAELYS_MCP_OK the slot holds one reference on `launcher`. On any failure
 * it holds none - including the unusable-descriptor path, where one was taken
 * and given back before returning.
 */
maelys_mcp_result_t maelys_mcp_process_launch(
    maelys_mcp_process_launcher_t *launcher,
    const maelys_mcp_process_launch_params_t *params,
    maelys_mcp_process_slot_t *out_slot,
    char **out_error);

/*
 * The bounded shutdown ladder, every rung budgeted:
 *
 *     stop(GRACEFUL) -> wait(grace_timeout_ms) -> stop(FORCED)
 *                    -> wait(force_timeout_ms) -> release
 *
 * The kind-specific goodbye - a provider/shutdown exchange for a native
 * provider, a half-close for an MCP upstream - happens ABOVE this call and
 * stays unmerged: those are protocol facts, not launch facts.
 *
 * A failed rung does not end the ladder; the runtime keeps climbing, because a
 * stop that could not be delivered is exactly when the next rung matters most.
 * The SHARPEST diagnostic seen is kept rather than the last one, and surfaces
 * in *out_error if the ladder ends in a containment failure.
 *
 * Returns MAELYS_MCP_OK when the child was observed to exit, and
 * MAELYS_MCP_ERR_STATE with a diagnostic in *out_error when it outlived a
 * forced stop. That second case is a containment failure and a genuinely bad
 * outcome; the contract chooses the leak over the hang and requires it to be
 * reported. release is called either way, exactly once, the provider's
 * reference on the launcher is dropped either way, and this function always
 * returns.
 *
 * A no-op returning MAELYS_MCP_OK on a slot that is not live, which is what
 * makes it safe to call after maelys_mcp_process_abandon has already run.
 */
maelys_mcp_result_t maelys_mcp_process_shutdown(
    maelys_mcp_process_slot_t *slot,
    unsigned int grace_timeout_ms,
    unsigned int force_timeout_ms,
    char **out_error);

/*
 * The failure-path form of the ladder: stop(FORCED) -> wait -> release, with
 * no graceful rung. Used where a launch has already failed for a reason of the
 * runtime's own (an allocation, a thread that would not start) and the child
 * is seconds old, has produced nothing to flush, and must not be given the
 * chance to ignore a polite request - the rule PR #53 established for exactly
 * these paths. Void, because these callers already have the error they are
 * returning. Drops the provider's reference on the launcher, like the full
 * ladder does.
 */
void maelys_mcp_process_abandon(
    maelys_mcp_process_slot_t *slot,
    unsigned int force_timeout_ms);
