#pragma once

#include <stddef.h>

#include "maelys/mcp/error.h"
#include "maelys/mcp/provider.h"
#include "maelys/mcp/version.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The seam every child process this runtime starts goes through: native
 * maelys-provider children and mcp-proxy upstreams alike.
 *
 * It exists so that confinement - a policy enforcement point, a virtualized
 * target, a launch service on another host - can be installed once and
 * govern every launch. A second fork/exec site would be a second bypass, so
 * scripts/audit_boundaries.sh forbids process-creation primitives outside
 * src/process/, and both provider kinds reach the substrate only through the
 * four ops below. Nothing in this runtime knows what confinement is; a
 * launcher does.
 *
 * The full contract - ownership, the bounded termination ladder, and the
 * parts of it the runtime states but cannot enforce - is
 * docs/launch-contract-design.md, whose "ABI 5" section is this revision's
 * rationale and its defect-by-defect argument.
 *
 * STATUS. This header is the ABI 5 contract, frozen; the implementation
 * behind it is milestone M2 and is not in the tree yet, so this branch does
 * not build. That order is deliberate rather than accidental: a launcher
 * maintained outside this repository is compiled against this header before
 * the runtime half exists, which makes the header the whole agreement
 * between the two, and makes a later correction to it expensive. Everything
 * stated here is stated because someone has to be able to implement it from
 * the header alone.
 *
 * ABI note. ABI 5 changes released layouts rather than adding new structures
 * beside them - maelys_mcp_process_spec_t and maelys_mcp_process_instance_t
 * are gone, and maelys_mcp_process_ops_t has a new shape - so
 * MAELYS_MCP_ABI_VERSION moves 4 -> 5 (docs/abi-policy.md). There is still
 * deliberately no struct_size field anywhere here: this project wants a
 * checkable ABI version and a refusal, not silent tolerance of mismatched
 * builds. That is what maelys_mcp_process_ops_t.abi_version and the check in
 * maelys_mcp_process_launcher_create are for.
 *
 * Error strings. Every `char **out_error` below follows the repo
 * convention: the caller passes either NULL, meaning "no message wanted",
 * or a pointer to a `char *` it has initialized; the callee writes a message
 * there only when it returns non-OK; and whatever it finds there afterwards
 * is the CALLER'S to free() exactly once. A callee with nothing to say
 * leaves the pointer alone. NULL is always legal to pass, at every op, and a
 * launcher must never dereference an out_error it was not given.
 */

/*
 * Which descriptor arrangement the child gets. NOT operator configuration: it
 * is a function of the child's protocol type, derived by the runtime and read
 * back by a launcher through maelys_mcp_process_request_fd_layout.
 *
 * STDIO is the zero value, and is the arrangement both providers shipped
 * with:
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
 * given ISOLATED even by mistake.
 */
typedef enum maelys_mcp_process_fd_layout {
    MAELYS_MCP_PROCESS_FD_STDIO = 0,
    MAELYS_MCP_PROCESS_FD_ISOLATED = 1
} maelys_mcp_process_fd_layout_t;

typedef enum maelys_mcp_process_stop {
    /* Ask. SIGTERM on POSIX; whatever the substrate's polite request is. */
    MAELYS_MCP_PROCESS_STOP_GRACEFUL = 0,
    /* Insist. SIGKILL on POSIX. Must not block on the child. */
    MAELYS_MCP_PROCESS_STOP_FORCED = 1
} maelys_mcp_process_stop_t;

/*
 * How a child ended, in full.
 *
 * ABI 4 asked `wait` for a single int - did it exit - and that boolean was
 * the whole diagnostic. "The provider died" and "the provider was killed by
 * SIGKILL after ignoring SIGTERM" reached the operator as the same sentence,
 * and the difference between them is the entire content of a shutdown
 * report. The status is carried now, at the one op that can know it.
 */
typedef struct maelys_mcp_process_exit_status {
    /* 1 if the launcher OBSERVED termination within the budget it was given,
     * 0 if it did not. 0 is not "still running" in any stronger sense than
     * that: the launcher looked, inside the budget, and did not see an end. */
    int exited;
    /* The child's exit status when it ended by returning or exiting; 0
     * otherwise. Meaningless unless `exited` is 1. */
    int exit_code;
    /* The signal that ended it, when a signal did and the substrate has
     * signals; 0 otherwise. Meaningless unless `exited` is 1. A substrate
     * with no notion of signals leaves this 0 and says everything it has to
     * say in exit_code. Exactly one of the two fields carries the outcome; a
     * launcher never reports both. */
    int term_signal;
} maelys_mcp_process_exit_status_t;

/*
 * The token maelys_mcp_process_request_environment_platform returns for a
 * launch on this process's own machine and platform. Spelled as a macro so
 * that both sides of the seam compare against the same characters.
 */
#define MAELYS_MCP_PROCESS_ENVIRONMENT_PLATFORM_LOCAL "local"

/*
 * What to launch. Built by the RUNTIME, never by an MCP request
 * (docs/security-model.md), and opaque: a launcher consults it through the
 * getters below and through nothing else.
 *
 * Opaque because this is the structure the contract will keep extending. As
 * a public plain-data struct - ABI 4's maelys_mcp_process_spec_t - every new
 * launch fact was an ABI break, which in practice meant new launch facts did
 * not get added and launchers re-derived them privately instead. Behind
 * getters, adding one is a new entry point, which is this project's idiom
 * for compatible extension (docs/abi-policy.md): a launcher built against an
 * older getter set keeps compiling, keeps linking and keeps working.
 *
 * EVERY POINTER A GETTER RETURNS IS VALID ONLY FOR THE DURATION OF THE spawn
 * CALL IN WHICH IT WAS OBTAINED. The request is not refcounted and cannot be
 * retained; it may cease to exist the instant spawn returns, on the success
 * path and the failure path alike. A launcher that keeps the executable
 * path, an argument, the profile, the platform token, or any environment
 * name or value beyond that point MUST COPY IT. This rule is stated once,
 * here, and holds for every getter without being repeated at each one.
 *
 * `request` is never NULL: the runtime does not hand a launcher a NULL
 * request, and the getters have no error channel with which to say it did.
 */
typedef struct maelys_mcp_process_request maelys_mcp_process_request_t;

/* Absolute, like every other executable path this runtime accepts. Never
 * NULL, never empty. */
const char *maelys_mcp_process_request_executable(
    const maelys_mcp_process_request_t *request);

/*
 * The COMPLETE argument vector, argv[0] included: arg_count is at least 1
 * and arg_at(0) is argv[0]. Never the "extra arguments" a manifest writes -
 * a launcher that reorders, prepends or interprets the vector is broken; it
 * passes it to the substrate as given.
 *
 * arg_at returns NULL for index >= arg_count. There is no terminating NULL
 * element to borrow: a launcher that needs an execve-shaped vector builds
 * one, which is work it was going to do anyway for any target that is not a
 * plain local exec.
 */
size_t maelys_mcp_process_request_arg_count(
    const maelys_mcp_process_request_t *request);

const char *maelys_mcp_process_request_arg_at(
    const maelys_mcp_process_request_t *request, size_t index);

/*
 * Opaque to the runtime, which never parses, compares or branches on it.
 * NULL means "the launcher's own default". The POSIX launcher accepts NULL
 * and "trusted-local" and refuses everything else, loudly - a manifest
 * asking for confinement must not be answered with an unconfined process and
 * no diagnostic, and that refusal is the launcher's to make because only the
 * launcher knows which profiles it implements.
 */
const char *maelys_mcp_process_request_execution_profile(
    const maelys_mcp_process_request_t *request);

/* Derived by the runtime from the child's protocol type, never configured.
 * See maelys_mcp_process_fd_layout_t. */
maelys_mcp_process_fd_layout_t maelys_mcp_process_request_fd_layout(
    const maelys_mcp_process_request_t *request);

/*
 * The budget spawn itself gets. A stated contract obligation, not an
 * enforced one: a fork-and-exec launcher cannot block and satisfies it
 * trivially, while a launcher talking to a service on another host can block
 * and must honour it - and the runtime, calling spawn on the startup path,
 * has no thread to time it out with. See "Budgets the runtime states but
 * cannot enforce".
 */
unsigned int maelys_mcp_process_request_spawn_timeout_ms(
    const maelys_mcp_process_request_t *request);

/* The two rungs of the shutdown ladder, separately budgeted: a polite
 * request deserves longer than a kill does. The runtime, not the launcher,
 * runs the ladder and passes these to wait; they are readable here so a
 * launcher can size its own internal waiting against the same numbers. */
unsigned int maelys_mcp_process_request_grace_timeout_ms(
    const maelys_mcp_process_request_t *request);

unsigned int maelys_mcp_process_request_force_timeout_ms(
    const maelys_mcp_process_request_t *request);

/*
 * The environment the child is to receive: a closed allowlist compiled by
 * the runtime, in the order the runtime intends, with no duplicate names.
 * NOT a filtered inheritance of this process's environment, and not
 * extensible by a manifest - a child gets no ambient credential from this
 * process, and no caller-supplied or request-supplied variable is ever in it
 * (docs/security-model.md).
 *
 * It is carried across the seam rather than compiled privately by each
 * launcher because it is the runtime's decision and there should be one
 * answer to it. Under ABI 4 every launcher rebuilt the same allowlist for
 * itself, which is two implementations of one rule the day there are two
 * launchers, and the one that drifts is the one nobody tested.
 *
 * environment_at fills *out_name with a name containing no '=' and
 * *out_value with its value, and returns 1. For index >= environment_count
 * it writes neither and returns 0. Both out parameters must be non-NULL.
 */
size_t maelys_mcp_process_request_environment_count(
    const maelys_mcp_process_request_t *request);

int maelys_mcp_process_request_environment_at(
    const maelys_mcp_process_request_t *request,
    size_t index,
    const char **out_name,
    const char **out_value);

/*
 * The platform the child will run on, as a canonical token:
 * MAELYS_MCP_PROCESS_ENVIRONMENT_PLATFORM_LOCAL when the target is this
 * process's own machine and platform, and otherwise a token naming the
 * target. Never NULL and never empty; lowercase ASCII letters, digits, '-',
 * '.' and '_' only, so it is always safe to print verbatim in a diagnostic.
 *
 * Host configuration, never protocol (docs/security-model.md): nothing an
 * MCP request carries can reach it. The runtime produces it and does not
 * interpret it beyond recognizing LOCAL - exactly like execution_profile,
 * every other value means something to the launcher and nothing to this
 * library. The token set is therefore open and grows without an ABI change.
 *
 * THE ENVIRONMENT PLATFORM RULE. This is what the token decides. It is the
 * launcher's obligation to apply it; the runtime cannot check it.
 *
 *   LOCAL
 *       The environment above is copied EXACTLY, entry for entry, in order.
 *       Same platform, same meaning for every variable in it; there is
 *       nothing to decide and nothing to leave out.
 *
 *   ANY OTHER TOKEN
 *       The target is not this platform. Then, and only then:
 *
 *       - PROTOCOL and LOCALE variables are copied, and nothing else is.
 *       - PATH is PRODUCED BY THE TARGET PROFILE, never carried across.
 *         This process's PATH is a fact about this machine's filesystem and
 *         means nothing on the target; the entry the request carries is
 *         ignored rather than adapted.
 *       - ANY VARIABLE THE LAUNCHER CANNOT PLACE IN ONE OF THOSE CLASSES IS
 *         AN EXPLICIT REFUSAL. spawn fails with MAELYS_MCP_ERR_ARGUMENT and
 *         an *out_error naming the variable and the platform token. It is
 *         never guessed at, never silently dropped, and above all NEVER
 *         SILENTLY TRANSLATED.
 *
 * That last clause is the point of the whole rule, so it is worth the space:
 * a silent translation starts a child in an environment nobody wrote and
 * nobody can reproduce, and the failure it causes surfaces arbitrarily far
 * from the decision that caused it. A refusal is one message, at the launch,
 * naming the one variable that has no agreed meaning on the target. An
 * unforeseen incompatibility is precisely the case in which neither side has
 * the standing to guess, so neither side is permitted to.
 *
 * The two classes, stated so that both sides classify identically:
 *
 *   PROTOCOL  variables whose value THIS RUNTIME computes and whose meaning
 *             is this project's own private provider protocol. They are
 *             spelled MAELYS_*, and MAELYS_PROVIDER_FD - the ISOLATED
 *             layout's descriptor number - is the only one today.
 *   LOCALE    LANG, LC_ALL, and any other LC_* variable.
 *
 * Both classes are defined by the SHAPE OF THE NAME rather than by an
 * enumerated list, deliberately. A launcher can then classify a variable it
 * has never heard of, so a protocol variable this runtime adds later reaches
 * a cross-platform target without every launcher needing a release first -
 * which is sound precisely because a protocol variable's value is computed by
 * this runtime and means the same thing on any target. What is left over is
 * a name of neither shape: the case on which nobody has agreed anything, and
 * therefore the case that gets refused.
 */
const char *maelys_mcp_process_request_environment_platform(
    const maelys_mcp_process_request_t *request);

typedef struct maelys_mcp_process_ops {
    /*
     * MAELYS_MCP_ABI_VERSION as seen by the translation unit that DEFINED
     * this table. Set it to the macro, never to a literal.
     *
     * THE CONTRACT: maelys_mcp_process_launcher_create compares this field
     * against the MAELYS_MCP_ABI_VERSION the library itself was built with,
     * requires EXACT EQUALITY, and refuses anything else with
     * MAELYS_MCP_ERR_ARGUMENT and an *out_error naming both numbers. No
     * launcher is created, no context is taken over, no op is ever called.
     *
     * Not ">=" and not "near enough", in either direction. An ops table is
     * the one structure in this header that crosses a build boundary: a
     * launcher is compiled separately from the library, possibly against a
     * different release of this header, and the two only meet at run time. A
     * mismatched vtable is a call through a function pointer at the wrong
     * offset with the wrong signature, which is not a failure any later
     * check can catch. There is no version pair for which proceeding beats
     * refusing at create time.
     *
     * It is the first member for the same reason: it stays readable at a
     * fixed offset whatever the rest of the table has come to look like.
     */
    unsigned int abi_version;
    /*
     * Start it. On MAELYS_MCP_OK:
     *
     *   *out_protocol_fd is the duplex protocol transport, owned by the
     *   RUNTIME from that moment on; the launcher must never touch it again.
     *   Guaranteed by the launcher to be >= 0, blocking, and close-on-exec
     *   in the RUNTIME's process, and to be an arrangement on which the
     *   runtime's writes cannot raise SIGPIPE (SO_NOSIGPIPE where it exists,
     *   otherwise a socket the MSG_NOSIGNAL write path can use - not a
     *   pipe).
     *
     *   *out_handle is whatever the launcher needs to identify what it
     *   started, owned by the LAUNCHER. The runtime stores it, hands it back
     *   to wait, stop and release, and never dereferences, compares, prints
     *   or reaps it. It may be a boxed pid_t, an identifier issued by
     *   another process, or a small integer that points at nothing.
     *
     *   NULL IS A VALID HANDLE - a stateless launcher needs no per-instance
     *   state - so handle nullity is never a liveness marker. The runtime
     *   tracks liveness in its own private state, which is why ABI 4's
     *   instance_live field is absent from this header: it was runtime
     *   bookkeeping living in a structure the launcher could write, and
     *   there is no version of that which is not a hazard.
     *
     * On anything other than MAELYS_MCP_OK: neither out parameter is
     * written, nothing was left running, the launcher has already unwound
     * its own partial state, and the runtime MUST NOT call release.
     */
    maelys_mcp_result_t (*spawn)(
        void *context,
        const maelys_mcp_process_request_t *request,
        void **out_handle,
        int *out_protocol_fd,
        char **out_error);
    /*
     * Has it exited? MUST return within timeout_ms. Reaps if the substrate
     * needs reaping - this is the ONLY op that reaps, and the only one
     * permitted to block at all. Never called on the request path; only on
     * the shutdown ladder.
     *
     * On MAELYS_MCP_OK, *out_status is written in full; see
     * maelys_mcp_process_exit_status_t for what each field means and when it
     * means nothing. On any other result *out_status must not be relied on,
     * and *out_error carries the launcher's account of why it could not
     * answer.
     *
     * MAELYS_MCP_OK with .exited == 0 after a FORCED stop is a CONTAINMENT
     * FAILURE: the launcher could not terminate what it started. The ladder
     * turns it into MAELYS_MCP_ERR_STATE naming the launcher, and propagates
     * it rather than discarding it - the contract chooses a visible leaked
     * process over a hung teardown, and requires the leak to be reported.
     */
    maelys_mcp_result_t (*wait)(
        void *context,
        void *handle,
        unsigned int timeout_ms,
        maelys_mcp_process_exit_status_t *out_status,
        char **out_error);
    /*
     * Signal termination. MUST NOT block on the child. Idempotent: the
     * ladder calls it twice on a stubborn child, and a stop after exit is
     * not an error.
     *
     * A failure here does not end the ladder. The runtime keeps climbing -
     * a stop that could not be delivered is exactly when the next rung
     * matters most - and keeps the best diagnostic it has seen rather than
     * letting a later, blander failure overwrite an earlier, sharper one.
     */
    maelys_mcp_result_t (*stop)(
        void *context,
        void *handle,
        maelys_mcp_process_stop_t mode,
        char **out_error);
    /*
     * LOCAL RELEASE ONLY. Frees the handle and any launcher-side memory,
     * closes launcher-private descriptors, unregisters bookkeeping.
     *
     * It MUST NOT wait for the child, MUST NOT signal it, and MUST NOT block
     * for an unbounded time. It is the one op with no timeout parameter,
     * precisely because it is not allowed to need one: a bound is worth
     * nothing if the last rung can hang, and a child that survived a forced
     * stop is exactly the case in which a tidy-looking reap at the end
     * parks teardown forever.
     *
     * Called EXACTLY ONCE per spawn that returned MAELYS_MCP_OK, and never
     * for one that did not; the runtime guarantees this from its own private
     * state. A handle is invalid the instant this returns. Void, because an
     * op that cannot fail in an interesting way has nothing to report - the
     * diagnostics come from wait, which can.
     */
    void (*release)(void *context, void *handle);
} maelys_mcp_process_ops_t;

/*
 * A launcher: an ops table, its context and a name, behind one opaque
 * refcounted handle.
 *
 * Refcounted because ABI 4's launcher was a borrowed pointer carrying a
 * written obligation - "must outlive the provider" - and no mechanism to
 * keep it. An embedder that spawned three providers from one launcher and
 * then freed it got a use-after-free at the first teardown, and every entry
 * point taking a launcher was one more place to get that right by hand.
 *
 * THE LIFETIME RULE THIS FIXES: the RUNTIME RETAINS PER PROVIDER. Each
 * provider spawned with a launcher takes its own reference and releases it
 * exactly once when the provider is destroyed. The caller's reference is
 * independent of all of them, so releasing it immediately after spawning is
 * correct and no ordering between the caller's release and any provider's
 * teardown can be wrong.
 *
 * Opaque because a launcher now owns something (its context, its refcount)
 * and a structure an embedder can write is a structure an embedder can
 * corrupt - and because the name has to be readable after the ops table it
 * came with is gone.
 */
typedef struct maelys_mcp_process_launcher maelys_mcp_process_launcher_t;

/*
 * Build one, holding one reference.
 *
 * `name` is for diagnostics - the runtime never branches on it, but it does
 * print it, so it must be non-NULL and non-empty. It is COPIED; the caller
 * keeps ownership of what it passed and may free it on return.
 *
 * `ops` must be non-NULL, every one of its four function pointers must be
 * non-NULL, and it is BORROWED rather than copied: it must outlive the
 * launcher. That is what lets an ops table live in .rodata and be shared by
 * every launcher an embedder creates.
 *
 * `context` is the launcher's own state, handed back to every op.
 * `release_context`, when non-NULL, is called exactly once with `context`
 * when the last reference goes away, on whichever thread happened to drop
 * it. NULL means the launcher owns nothing and there is nothing to release.
 *
 * ops->abi_version is verified here against MAELYS_MCP_ABI_VERSION; see the
 * member for what exactly is checked and why it is exact equality.
 *
 * On MAELYS_MCP_OK, *out_launcher holds ONE reference, which the caller
 * releases exactly once with maelys_mcp_process_launcher_release. On any
 * failure - including the ABI mismatch - *out_launcher is untouched, no
 * reference exists, `release_context` is NOT called and the caller still
 * owns its context, and *out_error, when set, is the caller's to free.
 */
maelys_mcp_result_t maelys_mcp_process_launcher_create(
    const char *name,
    const maelys_mcp_process_ops_t *ops,
    void *context,
    void (*release_context)(void *context),
    maelys_mcp_process_launcher_t **out_launcher,
    char **out_error);

/* One more reference. Thread-safe. NULL is a no-op. */
void maelys_mcp_process_launcher_retain(
    maelys_mcp_process_launcher_t *launcher);

/*
 * One fewer reference. At zero, `release_context` runs - once - and the
 * launcher is freed; the ops table is never freed, because it was borrowed.
 * NULL is a no-op.
 *
 * Thread-safe, and not merely as a convenience: the last reference is
 * dropped by whichever provider teardown finishes last, which is not in
 * general the thread that created the launcher.
 */
void maelys_mcp_process_launcher_release(
    maelys_mcp_process_launcher_t *launcher);

/*
 * The name given at creation, valid for as long as the caller holds a
 * reference. Diagnostics only: the ladder's containment failure names the
 * launcher that could not terminate what it started, and with an opaque
 * handle this is the only way to say which one it was.
 */
const char *maelys_mcp_process_launcher_name(
    const maelys_mcp_process_launcher_t *launcher);

/*
 * The launcher that reproduces the behaviour both providers shipped with: a
 * socketpair, a fork, the arrangement named by the request's fd layout, the
 * request's environment applied under the LOCAL rule, and an execve on this
 * machine. It applies no confinement and says so, by accepting only an
 * absent execution profile or the exact string "trusted-local" and refusing
 * everything else.
 *
 * Returns a new reference, exactly like any other launcher. ABI 4's
 * process-lifetime singleton is gone deliberately: one lifetime rule that
 * always holds is easier to keep than one rule plus an exception that only
 * the stock launcher enjoys.
 */
maelys_mcp_result_t maelys_mcp_posix_launcher_create(
    maelys_mcp_process_launcher_t **out_launcher,
    char **out_error);

/*
 * The provider entry points that take a launcher. maelys_mcp_provider_spawn,
 * maelys_mcp_provider_spawn_with_options, maelys_mcp_provider_spawn_with_args
 * and maelys_mcp_provider_proxy_spawn are unchanged in behaviour; each binds
 * a POSIX launcher of its own and releases it with the provider.
 *
 * `launcher` must be non-NULL - passing NULL is MAELYS_MCP_ERR_ARGUMENT
 * rather than a silent fall back to the POSIX launcher, because the point of
 * these entry points is to name the launcher, so a caller who names nothing
 * has made a mistake worth hearing about.
 *
 * On MAELYS_MCP_OK the provider holds its OWN reference on the launcher and
 * releases it exactly once when it is destroyed; the caller's reference is
 * unaffected and may be released at any time. On failure no reference is
 * taken. This is the whole of the launcher lifetime obligation these entry
 * points used to place on the caller.
 */
maelys_mcp_result_t maelys_mcp_provider_spawn_with_launcher(
    const maelys_mcp_provider_process_options_t *options,
    maelys_mcp_process_launcher_t *launcher,
    maelys_mcp_provider_t **out_provider,
    char **out_error);

maelys_mcp_result_t maelys_mcp_provider_proxy_spawn_with_launcher(
    const maelys_mcp_proxy_options_t *options,
    maelys_mcp_process_launcher_t *launcher,
    maelys_mcp_provider_t **out_provider,
    char **out_skipped_tools,
    char **out_error);

#ifdef __cplusplus
}
#endif
