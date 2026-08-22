#include "src/internal/internal.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The runtime's half of the launch contract: the refcounted launcher, the
 * compiled request, the ownership rules, the exactly-once teardown and the
 * bounded ladder, implemented once for every consumer. Deliberately free of
 * process primitives - it only calls through the vtable, which is what lets an
 * OCI, a VM or an executord launcher fit without a line changing here.
 */

static void set_launch_error(char **out_error, const char *message) {
    if (!out_error) return;
    free(*out_error);
    *out_error = maelys_mcp_strdup(message);
}

/*
 * Diagnostics about a launcher name it. The name is the launcher's own
 * (arbitrary) string, so the buffer is fixed and the format truncates rather
 * than sizing itself from untrusted input.
 */
static void set_launcher_error(
    char **out_error,
    const maelys_mcp_process_launcher_t *launcher,
    const char *message) {
    if (!out_error) return;
    char text[256];
    const char *name = maelys_mcp_process_launcher_name(launcher);
    (void)snprintf(text, sizeof(text), "%s (launcher \"%s\")", message,
        name ? name : "unnamed");
    set_launch_error(out_error, text);
}

/* ------------------------------------------------------------- the launcher */

maelys_mcp_result_t maelys_mcp_process_launcher_create(
    const char *name,
    const maelys_mcp_process_ops_t *ops,
    void *context,
    void (*release_context)(void *context),
    maelys_mcp_process_launcher_t **out_launcher,
    char **out_error) {
    if (!out_launcher) {
        set_launch_error(out_error,
            "process launcher creation needs somewhere to put the launcher");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    if (!name || !*name) {
        set_launch_error(out_error, "process launcher name must not be empty");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    if (!ops) {
        set_launch_error(out_error, "process launcher ops table must not be null");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    /*
     * Before the function pointers, and not merely for tidiness: an ops table
     * is the one structure in the public header that crosses a build boundary,
     * so if the version disagrees then every offset after the first member is
     * a guess, and "are the four pointers non-NULL" is a question asked of the
     * wrong bytes. abi_version is the first member precisely so that it stays
     * readable whatever the rest of the table has come to look like, and this
     * is the check that makes that property worth having.
     *
     * Exact equality, in either direction. A mismatched vtable is a call
     * through a function pointer at the wrong offset with the wrong signature,
     * which no later check catches and no sanitizer report explains; there is
     * no version pair for which proceeding beats refusing here.
     */
    if (ops->abi_version != MAELYS_MCP_ABI_VERSION) {
        char text[256];
        (void)snprintf(text, sizeof(text),
            "process launcher \"%s\" was built against ABI version %u, "
            "this library implements ABI version %u",
            name, ops->abi_version, (unsigned int)MAELYS_MCP_ABI_VERSION);
        set_launch_error(out_error, text);
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    if (!ops->spawn || !ops->wait || !ops->stop || !ops->release) {
        set_launch_error(out_error,
            "process launcher ops table is missing an operation");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    maelys_mcp_process_launcher_t *launcher = calloc(1u, sizeof(*launcher));
    if (!launcher) return MAELYS_MCP_ERR_MEMORY;
    launcher->name = maelys_mcp_strdup(name);
    if (!launcher->name) {
        free(launcher);
        return MAELYS_MCP_ERR_MEMORY;
    }
    /*
     * Borrowed, never copied: the table may live in .rodata and be shared by
     * every launcher an embedder creates, which is only true if this does not
     * take a copy of it.
     */
    launcher->ops = ops;
    launcher->context = context;
    launcher->release_context = release_context;
    /* The one reference the caller is handed and releases exactly once. */
    atomic_init(&launcher->references, 1u);
    *out_launcher = launcher;
    return MAELYS_MCP_OK;
}

void maelys_mcp_process_launcher_retain(
    maelys_mcp_process_launcher_t *launcher) {
    if (!launcher) return;
    /* Relaxed is enough to add a reference: the caller already holds one, so
     * the object cannot be going away underneath this. */
    atomic_fetch_add_explicit(&launcher->references, 1u, memory_order_relaxed);
}

void maelys_mcp_process_launcher_release(
    maelys_mcp_process_launcher_t *launcher) {
    if (!launcher) return;
    /*
     * acq_rel, because the thread that drops the last reference has to see
     * everything every other reference-holder did before dropping theirs -
     * release_context runs here, on that thread, and it is entitled to a
     * consistent view of whatever the context accumulated. The last reference
     * is dropped by whichever provider teardown finishes last, which is not in
     * general the thread that created the launcher.
     */
    if (atomic_fetch_sub_explicit(&launcher->references, 1u,
        memory_order_acq_rel) != 1u) {
        return;
    }
    /* Exactly once, at zero, on whichever thread happened to get here. The ops
     * table is not freed: it was borrowed. */
    if (launcher->release_context) launcher->release_context(launcher->context);
    free(launcher->name);
    free(launcher);
}

const char *maelys_mcp_process_launcher_name(
    const maelys_mcp_process_launcher_t *launcher) {
    return launcher ? launcher->name : NULL;
}

/* -------------------------------------------------------------- the request */

/*
 * The environment the child is to receive, compiled here and nowhere else: a
 * closed allowlist, in the order the runtime intends, with no duplicate names.
 * NOT a filtered inheritance of this process's environment, and not extensible
 * by a manifest - a child gets no ambient credential from this process, and no
 * caller-supplied or request-supplied variable is ever in it
 * (docs/security-model.md).
 *
 * Every value is a string literal, so this allocates nothing and cannot fail.
 */
static void compile_environment(
    maelys_mcp_process_request_t *request,
    maelys_mcp_process_fd_layout_t fd_layout) {
    size_t count = 0u;
    request->environment_names[count] = "PATH";
    request->environment_values[count++] = MAELYS_MCP_PROCESS_CHILD_PATH;
    request->environment_names[count] = "LANG";
    request->environment_values[count++] = "C";
    request->environment_names[count] = "LC_ALL";
    request->environment_values[count++] = "C";
    /*
     * Only under ISOLATED, and only for that layout's sake: it tells a
     * first-party SDK where the protocol went when it is not on stdin and
     * stdout. Adding it unconditionally would tell a STDIO child to look at a
     * descriptor it was never given.
     */
    if (fd_layout == MAELYS_MCP_PROCESS_FD_ISOLATED) {
        request->environment_names[count] = "MAELYS_PROVIDER_FD";
        request->environment_values[count++] =
            MAELYS_MCP_PROCESS_ISOLATED_FD_TEXT;
    }
    request->environment_count = count;
}

static maelys_mcp_result_t compile_request(
    maelys_mcp_process_request_t *request,
    const maelys_mcp_process_launch_params_t *params) {
    memset(request, 0, sizeof(*request));
    if (!params->executable_path || !*params->executable_path ||
        !params->argv || !params->argv[0]) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    size_t arg_count = 0u;
    while (params->argv[arg_count]) ++arg_count;
    request->executable = params->executable_path;
    request->argv = params->argv;
    request->arg_count = arg_count;
    request->execution_profile = params->execution_profile;
    request->fd_layout = params->fd_layout;
    request->spawn_timeout_ms = params->spawn_timeout_ms;
    request->grace_timeout_ms = params->grace_timeout_ms;
    request->force_timeout_ms = params->force_timeout_ms;
    /*
     * The only token this runtime produces today: every launch it compiles is
     * for a child on this process's own machine and platform. A launcher that
     * targets something else gets its token from host configuration, never
     * from protocol (docs/security-model.md), and that plumbing is M3's.
     */
    request->environment_platform =
        MAELYS_MCP_PROCESS_ENVIRONMENT_PLATFORM_LOCAL;
    compile_environment(request, params->fd_layout);
    return MAELYS_MCP_OK;
}

/* --------------------------------------------------------------- the ladder */

static int launcher_is_complete(
    const maelys_mcp_process_launcher_t *launcher) {
    /* A launcher that exists was checked at creation; this is the NULL guard
     * the entry points need, in the shape the ladder already used. */
    return launcher && launcher->ops && launcher->ops->spawn &&
        launcher->ops->wait && launcher->ops->stop && launcher->ops->release;
}

/*
 * Cheap, once per provider, and it catches the realistic mistake - an
 * uninitialized or already-closed descriptor - without pretending to catch
 * every one.
 */
static int protocol_fd_is_usable(int fd) {
    return fd >= 0 && fcntl(fd, F_GETFD) >= 0;
}

/*
 * The ladder keeps the FIRST diagnostic it saw and discards later ones. A stop
 * that could not be delivered explains the containment failure that follows it
 * far better than the wait that merely reports the budget was spent, and the
 * later message is always the blander of the two - so "sharpest" and "first"
 * are the same message, and first is the one that can be selected without
 * ranking prose.
 */
static void keep_sharpest(char **kept, char *candidate) {
    if (!candidate) return;
    if (*kept) {
        free(candidate);
        return;
    }
    *kept = candidate;
}

/* stop -> wait, one rung. Reports whether the child was observed to exit, and
 * collects whatever either op had to say about why it could not. */
static int stop_and_wait(
    maelys_mcp_process_launcher_t *launcher,
    void *handle,
    maelys_mcp_process_stop_t mode,
    unsigned int timeout_ms,
    char **kept_error) {
    char *stop_error = NULL;
    if (launcher->ops->stop(launcher->context, handle, mode, &stop_error) !=
        MAELYS_MCP_OK) {
        keep_sharpest(kept_error, stop_error);
    } else {
        /* An op that returned OK leaves the pointer alone, so this is
         * free(NULL) unless a launcher wrote a message it had no business
         * writing - in which case it is the leak that message would have
         * been. */
        free(stop_error);
    }
    char *wait_error = NULL;
    maelys_mcp_process_exit_status_t status = {
        .exited = 0, .exit_code = 0, .term_signal = 0
    };
    if (launcher->ops->wait(launcher->context, handle, timeout_ms, &status,
        &wait_error) != MAELYS_MCP_OK) {
        /* On a non-OK wait the status must not be relied on, so it is not
         * read: a launcher that could not answer has told us nothing about
         * whether the child ended. */
        keep_sharpest(kept_error, wait_error);
        return 0;
    }
    free(wait_error);
    return status.exited;
}

/*
 * The one place release is called. `live` is cleared BEFORE the call, not
 * after, so a release that itself re-enters teardown cannot reach a second one
 * - and so nullity of the handle, which is a legitimate value for a stateless
 * launcher, is never mistaken for "already released".
 *
 * The provider's reference on the launcher goes back here too, after the op:
 * the ops table is reached through the launcher, so the launcher has to
 * outlive the call that uses it.
 */
static void release_slot(maelys_mcp_process_slot_t *slot) {
    maelys_mcp_process_launcher_t *launcher = slot->launcher;
    void *handle = slot->handle;
    slot->live = 0;
    slot->handle = NULL;
    slot->launcher = NULL;
    slot->protocol_fd = -1;
    launcher->ops->release(launcher->context, handle);
    maelys_mcp_process_launcher_release(launcher);
}

maelys_mcp_result_t maelys_mcp_process_launch(
    maelys_mcp_process_launcher_t *launcher,
    const maelys_mcp_process_launch_params_t *params,
    maelys_mcp_process_slot_t *out_slot,
    char **out_error) {
    if (!launcher_is_complete(launcher) || !params || !out_slot) {
        set_launch_error(out_error, "process launcher is incomplete");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    maelys_mcp_process_request_t request;
    if (compile_request(&request, params) != MAELYS_MCP_OK) {
        set_launch_error(out_error, "process launch request is incomplete");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    void *handle = NULL;
    int protocol_fd = -1;
    maelys_mcp_result_t status = launcher->ops->spawn(launcher->context,
        &request, &handle, &protocol_fd, out_error);
    /*
     * `request` dies with this frame, on both paths. That is the whole of the
     * header's lifetime rule, enforced by scope: a launcher that kept one of
     * its strings kept a pointer into a dead stack frame, and there is no
     * runtime change that could have made that legal.
     */
    if (status != MAELYS_MCP_OK) return status;
    /*
     * From here the launch succeeded as far as the launcher is concerned, so
     * the child is the runtime's problem even when the runtime rejects what it
     * was handed. The slot is made live first, and the provider's reference
     * taken first: every path below tears down through the ladder, and the
     * ladder refuses to act on a slot that is not live.
     */
    maelys_mcp_process_launcher_retain(launcher);
    maelys_mcp_process_slot_t slot = {
        .launcher = launcher,
        .handle = handle,
        .protocol_fd = protocol_fd,
        .live = 1
    };
    if (!protocol_fd_is_usable(protocol_fd)) {
        /*
         * The one case where both sides could be right, resolved in the
         * runtime's favour because the runtime is the only side that can
         * detect it. Nothing valid to close, and the child is stopped rather
         * than leaked. Loud, because "the runtime survives a broken launcher"
         * is exactly the property a pluggable seam has to earn.
         *
         * The message is composed before the abandon, because the abandon
         * gives back the reference this launch took, and the name it prints
         * belongs to the launcher.
         */
        set_launcher_error(out_error, launcher,
            "process launcher returned an unusable protocol descriptor");
        maelys_mcp_process_abandon(&slot, params->force_timeout_ms);
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    *out_slot = slot;
    return MAELYS_MCP_OK;
}

maelys_mcp_result_t maelys_mcp_process_shutdown(
    maelys_mcp_process_slot_t *slot,
    unsigned int grace_timeout_ms,
    unsigned int force_timeout_ms,
    char **out_error) {
    if (!slot || !slot->live || !launcher_is_complete(slot->launcher)) {
        return MAELYS_MCP_OK;
    }
    maelys_mcp_process_launcher_t *launcher = slot->launcher;
    void *handle = slot->handle;
    char *kept_error = NULL;
    int exited = stop_and_wait(launcher, handle,
        MAELYS_MCP_PROCESS_STOP_GRACEFUL, grace_timeout_ms, &kept_error);
    if (!exited) {
        exited = stop_and_wait(launcher, handle,
            MAELYS_MCP_PROCESS_STOP_FORCED, force_timeout_ms, &kept_error);
    }
    maelys_mcp_result_t status = MAELYS_MCP_OK;
    if (!exited) {
        /*
         * The child outlived a forced stop. Something has to give, and the
         * honest options are a hang or a leak: this contract chooses the leak
         * and requires it to be reported. A leaked process is visible in ps,
         * survivable and attributable; a hung teardown is an unkillable host
         * with no message.
         *
         * If a rung had something specific to say, that is what gets reported:
         * "the kill could not be delivered" is a diagnosis, where "it survived
         * a forced stop" is only the symptom.
         */
        set_launcher_error(out_error, launcher, kept_error ? kept_error :
            "child process survived a forced stop and was abandoned");
        status = MAELYS_MCP_ERR_STATE;
    }
    free(kept_error);
    release_slot(slot);
    return status;
}

void maelys_mcp_process_abandon(
    maelys_mcp_process_slot_t *slot,
    unsigned int force_timeout_ms) {
    if (!slot || !slot->live || !launcher_is_complete(slot->launcher)) return;
    char *kept_error = NULL;
    (void)stop_and_wait(slot->launcher, slot->handle,
        MAELYS_MCP_PROCESS_STOP_FORCED, force_timeout_ms, &kept_error);
    /* These callers already have the error they are returning, and this path
     * is reached because something of the RUNTIME's failed - a diagnostic
     * about the child would displace the one that actually explains it. */
    free(kept_error);
    release_slot(slot);
}
