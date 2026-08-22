#include "src/internal/internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * The launcher that reproduces what src/provider/process_provider.c and
 * src/provider/mcp_proxy.c each used to do for themselves: a socketpair, a
 * fork, the peer end on stdin and stdout, the request's environment, execve.
 * It also implements MAELYS_MCP_PROCESS_FD_ISOLATED (native providers only -
 * see include/maelys/mcp/process_launcher.h): the peer end on fd 3 instead,
 * stdin on /dev/null, stdout wired to stderr, and MAELYS_PROVIDER_FD=3 already
 * in the environment the runtime compiled.
 *
 * Under ABI 5 it is also the first consumer of the seam's own contract rather
 * than an insider: it reads the launch through the public getters, builds its
 * own execve-shaped argv and envp from them, and applies the environment under
 * the platform rule like any other launcher would. That is deliberate. A stock
 * launcher that reached around the getters would leave the getters untested by
 * anything that ships, and the Executor adapter would be the first code to
 * find out whether they work.
 *
 * This is the only file in the tree permitted to call fork, execve, socketpair,
 * waitpid or kill; scripts/audit_boundaries.sh enforces that. Everything above
 * the seam sees the four ops in src/process/launcher.h and no PID at all.
 */

/*
 * The launcher's handle. Opaque above the seam - the runtime stores it and
 * hands it back, and must never dereference it - and reaped is the field that
 * keeps this launcher safe to call in any order: once a child has been reaped
 * its pid belongs to the kernel again and may already name someone else, so a
 * stop after a successful wait must not signal it.
 *
 * The ladder as written never does that - its second stop is reached only when
 * the first wait did NOT observe an exit, so nothing was reaped - and no test
 * can therefore distinguish this guard from its absence. It is kept anyway,
 * because the ops are a public contract and the rung order is not the only
 * legal way to call them: stop and wait are documented as idempotent, and a
 * launcher that signalled a recycled pid would be a very bad way to discover
 * that the documentation was aspirational.
 *
 * `status` is what makes wait idempotent in the ABI 5 sense: a child is
 * reapable exactly once, so the outcome is remembered at the reap and replayed
 * to every wait after it. Without it a second wait would have to answer "it
 * ended, and I have forgotten how", which is a worse answer than the one it
 * already has.
 */
typedef struct posix_child {
    pid_t pid;
    int reaped;
    maelys_mcp_process_exit_status_t status;
} posix_child_t;

static void set_posix_error(char **out_error, const char *message) {
    if (!out_error) return;
    free(*out_error);
    *out_error = maelys_mcp_strdup(message);
}

static int set_close_on_exec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static long long monotonic_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (long long)now.tv_sec * 1000LL + (long long)now.tv_nsec / 1000000LL;
}

typedef enum child_wait_outcome {
    /* Termination was observed, and *out_status describes it. */
    CHILD_WAIT_EXITED = 0,
    /* The budget was spent and the child is still there. Not an error. */
    CHILD_WAIT_TIMED_OUT,
    /* The question could not be answered at all. */
    CHILD_WAIT_FAILED
} child_wait_outcome_t;

/*
 * WNOHANG plus deadline polling, never a blocking waitpid: the bound is the
 * whole point of the ladder, and a child in uninterruptible sleep is unkillable
 * and would park teardown forever otherwise.
 *
 * *out_status must arrive zeroed; this only ever sets fields, so a caller
 * reading it after a non-EXITED outcome sees the zeroes it started with.
 */
static child_wait_outcome_t wait_for_child(
    pid_t pid,
    unsigned int timeout_ms,
    maelys_mcp_process_exit_status_t *out_status) {
    long long start = monotonic_milliseconds();
    if (start < 0) return CHILD_WAIT_FAILED;
    for (;;) {
        int raw_status = 0;
        pid_t waited = waitpid(pid, &raw_status, WNOHANG);
        if (waited == pid) {
            out_status->exited = 1;
            /*
             * Exactly one of the two carries the outcome, which is what the
             * contract requires and what waitpid's own macros already
             * guarantee: a status is either an exit or a signal, never both.
             */
            if (WIFEXITED(raw_status)) {
                out_status->exit_code = WEXITSTATUS(raw_status);
            } else if (WIFSIGNALED(raw_status)) {
                out_status->term_signal = WTERMSIG(raw_status);
            }
            return CHILD_WAIT_EXITED;
        }
        if (waited < 0 && errno == ECHILD) {
            /*
             * Someone else reaped it - an embedder with its own SIGCHLD
             * handler, most plausibly. The end was real and is reported as
             * such; the status is genuinely not ours to give, and leaving both
             * fields at zero says "it ended" without inventing a cause.
             */
            out_status->exited = 1;
            return CHILD_WAIT_EXITED;
        }
        if (waited < 0 && errno != EINTR) return CHILD_WAIT_FAILED;
        long long now = monotonic_milliseconds();
        if (now < 0) return CHILD_WAIT_FAILED;
        if (now - start >= (long long)timeout_ms) return CHILD_WAIT_TIMED_OUT;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000L};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
}

/*
 * Absent means "the launcher's own default", which for this one is no
 * confinement at all. "trusted-local" is the explicit spelling of the same
 * thing. Everything else is a request for confinement this launcher cannot
 * provide, and starting the child anyway would answer a security request with
 * an unsandboxed process and no diagnostic - so it refuses, and says which
 * profile it refused.
 */
static int profile_is_accepted(const char *profile) {
    return !profile || strcmp(profile, "trusted-local") == 0;
}

/*
 * This launcher forks on the machine it is running on, so the only environment
 * platform it can implement is this one. Every other token names a target it
 * cannot reach, and the contract's answer to a target a launcher does not
 * implement is an explicit refusal naming it - never a launch that quietly
 * happens somewhere else than the caller asked for.
 */
static int platform_is_local(const char *platform) {
    return platform &&
        strcmp(platform, MAELYS_MCP_PROCESS_ENVIRONMENT_PLATFORM_LOCAL) == 0;
}

static void free_vector(char **vector) {
    if (!vector) return;
    for (size_t index = 0; vector[index]; ++index) free(vector[index]);
    free(vector);
}

/*
 * The request's argument vector, in the NULL-terminated shape execve wants.
 * The contract carries a count rather than a terminator, so this is the work
 * it says a fork-and-exec launcher has to do - and every other kind of
 * launcher was going to re-encode the vector for its target anyway.
 *
 * The strings are BORROWED, not copied. They are valid for the whole of this
 * spawn call, the child's copy of this vector is made by fork, and the child
 * reaches execve without returning - so no pointer here outlives the frame it
 * came from. Only the array of pointers is this function's to free.
 */
static char **build_arguments(const maelys_mcp_process_request_t *request) {
    size_t count = maelys_mcp_process_request_arg_count(request);
    char **arguments = calloc(count + 1u, sizeof(*arguments));
    if (!arguments) return NULL;
    for (size_t index = 0; index < count; ++index) {
        arguments[index] =
            (char *)maelys_mcp_process_request_arg_at(request, index);
    }
    return arguments;
}

/*
 * The request's environment as execve's NAME=VALUE vector, applied under the
 * LOCAL rule: copied exactly, entry for entry, in order. Same platform, same
 * meaning for every variable in it; there is nothing to decide and nothing to
 * leave out. The non-local branch of the rule is not implemented here because
 * this launcher refuses non-local platforms outright, before it gets this far.
 *
 * Built in the PARENT, before fork. A forked child of a multithreaded process
 * may call only async-signal-safe functions until it execs, and malloc is not
 * one of them - so the concatenation happens while there is still only one
 * process to do it in.
 */
static char **build_environment(const maelys_mcp_process_request_t *request) {
    size_t count = maelys_mcp_process_request_environment_count(request);
    char **environment = calloc(count + 1u, sizeof(*environment));
    if (!environment) return NULL;
    for (size_t index = 0; index < count; ++index) {
        const char *name = NULL;
        const char *value = NULL;
        if (!maelys_mcp_process_request_environment_at(request, index, &name,
            &value)) {
            /*
             * Unreachable: the count is the request's own. Treated as a
             * failure rather than a break, because a break would leave a NULL
             * in the middle of the vector and execve reads that as the end -
             * silently starting the child with a truncated environment, which
             * is the one outcome the closed allowlist exists to prevent.
             */
            free_vector(environment);
            return NULL;
        }
        size_t size = strlen(name) + strlen(value) + 2u;
        char *entry = malloc(size);
        if (!entry) {
            free_vector(environment);
            return NULL;
        }
        (void)snprintf(entry, size, "%s=%s", name, value);
        environment[index] = entry;
    }
    return environment;
}

static maelys_mcp_result_t posix_spawn_child(
    void *context,
    const maelys_mcp_process_request_t *request,
    void **out_handle,
    int *out_protocol_fd,
    char **out_error) {
    (void)context;
    if (!request || !out_handle || !out_protocol_fd) {
        set_posix_error(out_error, "process launch request is incomplete");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    const char *profile = maelys_mcp_process_request_execution_profile(request);
    if (!profile_is_accepted(profile)) {
        char text[256];
        (void)snprintf(text, sizeof(text),
            "execution profile \"%s\" is not supported by the posix launcher",
            profile);
        set_posix_error(out_error, text);
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    const char *platform =
        maelys_mcp_process_request_environment_platform(request);
    if (!platform_is_local(platform)) {
        /* The token's grammar - lowercase ASCII, digits, '-', '.', '_' - is
         * fixed by the header precisely so that the one thing a launcher will
         * do with an unknown one is always safe to do. */
        char text[256];
        (void)snprintf(text, sizeof(text),
            "environment platform \"%s\" is not implemented by the posix "
            "launcher", platform ? platform : "");
        set_posix_error(out_error, text);
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    maelys_mcp_process_fd_layout_t fd_layout =
        maelys_mcp_process_request_fd_layout(request);
    if (fd_layout != MAELYS_MCP_PROCESS_FD_STDIO &&
        fd_layout != MAELYS_MCP_PROCESS_FD_ISOLATED) {
        set_posix_error(out_error,
            "the posix launcher does not implement this descriptor layout");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    const char *executable = maelys_mcp_process_request_executable(request);
    char **arguments = build_arguments(request);
    char **environment = arguments ? build_environment(request) : NULL;
    if (!arguments || !environment) {
        free(arguments);
        free_vector(environment);
        return MAELYS_MCP_ERR_MEMORY;
    }
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        free(arguments);
        free_vector(environment);
        set_posix_error(out_error, "socketpair failed");
        return MAELYS_MCP_ERR_IO;
    }
    /* Both ends, so the parent's copy cannot leak into an unrelated child; the
     * child clears the flag again on the descriptors it keeps. */
    if (!set_close_on_exec(sockets[0]) || !set_close_on_exec(sockets[1])) {
        close(sockets[0]);
        close(sockets[1]);
        free(arguments);
        free_vector(environment);
        set_posix_error(out_error, "cannot protect the child's descriptors");
        return MAELYS_MCP_ERR_IO;
    }
    posix_child_t *child = calloc(1u, sizeof(*child));
    if (!child) {
        close(sockets[0]);
        close(sockets[1]);
        free(arguments);
        free_vector(environment);
        return MAELYS_MCP_ERR_MEMORY;
    }
    pid_t pid = fork();
    if (pid < 0) {
        free(child);
        close(sockets[0]);
        close(sockets[1]);
        free(arguments);
        free_vector(environment);
        set_posix_error(out_error, "fork failed");
        return MAELYS_MCP_ERR_IO;
    }
    if (pid == 0) {
        close(sockets[0]);
        if (fd_layout == MAELYS_MCP_PROCESS_FD_ISOLATED) {
            /*
             * The protocol moves to fd 3 before fd 0/1/2 are touched at all:
             * if socketpair happened to hand back a low-numbered fd for the
             * child's end (0, 1 or 2), dup2'ing /dev/null or stderr onto that
             * number first would close the only copy of the socket this
             * child has, before it ever reaches fd 3.
             */
            if (dup2(sockets[1], MAELYS_MCP_PROCESS_ISOLATED_FD) < 0) _exit(126);
            if (sockets[1] != MAELYS_MCP_PROCESS_ISOLATED_FD) close(sockets[1]);
            /* Same oldfd == newfd hazard as the STDIO arrangement below,
             * applied to fd 3 instead of fd 0/1: dup2 is a no-op when
             * socketpair happened to hand back fd 3 itself, which would
             * leave FD_CLOEXEC set and make execve close the protocol end.
             * Cleared unconditionally, not only on the branch that believes a
             * duplication occurred. */
            if (fcntl(MAELYS_MCP_PROCESS_ISOLATED_FD, F_SETFD, 0) != 0) {
                _exit(126);
            }
            /* Nothing to read from stdin. An old SDK that predates
             * MAELYS_PROVIDER_FD reads immediate EOF here and exits fast,
             * which the runtime reports as a precise "death before describe"
             * rather than hanging to the describe deadline (the tolerability
             * argument in "Correcting the framing: this is a declaration,
             * not a negotiation"). */
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull < 0) _exit(126);
            if (dup2(devnull, STDIN_FILENO) < 0) _exit(126);
            if (devnull != STDIN_FILENO) close(devnull);
            /* stdout IS stderr at the kernel level: nothing this child or any
             * dependency it links - a printf, a native addon, fs.writeSync(1,
             * ...), a grandchild it spawns - can reach the protocol through
             * fd 1 anymore, whether or not its own SDK cooperates. */
            if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) _exit(126);
            execve(executable, arguments, environment);
            _exit(127);
        }
        if (dup2(sockets[1], STDIN_FILENO) < 0 ||
            dup2(sockets[1], STDOUT_FILENO) < 0) _exit(126);
        if (sockets[1] != STDIN_FILENO && sockets[1] != STDOUT_FILENO) {
            close(sockets[1]);
        }
        /* dup2 with oldfd == newfd is a POSIX no-op that leaves FD_CLOEXEC
         * set - execve would then close the protocol end, and the child
         * would start with stdin or stdout already gone. The line above
         * guards that same edge case for the close; this clears the flag.
         * For the common oldfd != newfd case dup2 already cleared it and
         * this is a harmless second clear. */
        if (fcntl(STDIN_FILENO, F_SETFD, 0) != 0 ||
            fcntl(STDOUT_FILENO, F_SETFD, 0) != 0) _exit(126);
        execve(executable, arguments, environment);
        _exit(127);
    }
    close(sockets[1]);
    /* The child has its own copy of both vectors, made by fork; these are the
     * parent's and have no further use. */
    free(arguments);
    free_vector(environment);
    child->pid = pid;
#ifdef SO_NOSIGPIPE
    /*
     * The BSD/macOS half of the no-SIGPIPE obligation; Linux has none of this
     * and relies on the runtime's MSG_NOSIGNAL write path instead
     * (src/core/common.c). Together they cover both platforms.
     */
    int no_sigpipe = 1;
    if (setsockopt(sockets[0], SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
        sizeof(no_sigpipe)) != 0) {
        close(sockets[0]);
        /* SIGKILL, not SIGTERM: the child is seconds old, has produced
         * nothing to flush, and a provider that ignores SIGTERM (a shell
         * wrapper, a runtime with a handler) would park this wait - and host
         * startup with it. The wait is bounded for the same reason. */
        (void)kill(pid, SIGKILL);
        maelys_mcp_process_exit_status_t discarded = {
            .exited = 0, .exit_code = 0, .term_signal = 0
        };
        (void)wait_for_child(pid,
            maelys_mcp_process_request_force_timeout_ms(request), &discarded);
        free(child);
        set_posix_error(out_error, "cannot configure the child's socket safety");
        return MAELYS_MCP_ERR_IO;
    }
#endif
    *out_protocol_fd = sockets[0];
    *out_handle = child;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t posix_wait(
    void *context,
    void *handle,
    unsigned int timeout_ms,
    maelys_mcp_process_exit_status_t *out_status,
    char **out_error) {
    (void)context;
    if (!out_status) {
        set_posix_error(out_error,
            "process wait needs somewhere to report the exit status");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    out_status->exited = 0;
    out_status->exit_code = 0;
    out_status->term_signal = 0;
    posix_child_t *child = handle;
    if (!child || child->pid <= 0) {
        /* Nothing was started that this launcher can account for, so there is
         * nothing still running; the cause is genuinely unknown and both
         * fields stay at zero. */
        out_status->exited = 1;
        return MAELYS_MCP_OK;
    }
    if (child->reaped) {
        /* Reapable exactly once, remembered from that reap: a repeat wait gets
         * the same full answer rather than a vaguer one. */
        *out_status = child->status;
        return MAELYS_MCP_OK;
    }
    maelys_mcp_process_exit_status_t observed = {
        .exited = 0, .exit_code = 0, .term_signal = 0
    };
    switch (wait_for_child(child->pid, timeout_ms, &observed)) {
        case CHILD_WAIT_EXITED:
            child->reaped = 1;
            child->status = observed;
            *out_status = observed;
            return MAELYS_MCP_OK;
        case CHILD_WAIT_TIMED_OUT:
            /* Not an error of this op's own: the budget expired and the child
             * is still there. The ladder decides what that means, and only the
             * rung after a forced stop makes it a containment failure. */
            return MAELYS_MCP_OK;
        case CHILD_WAIT_FAILED:
        default:
            /* This one IS an error of its own: the launcher cannot answer the
             * question at all, which is a different thing from answering "not
             * yet" and used to be indistinguishable from it. */
            set_posix_error(out_error,
                "cannot determine whether the child process ended");
            return MAELYS_MCP_ERR_IO;
    }
}

static maelys_mcp_result_t posix_stop(
    void *context,
    void *handle,
    maelys_mcp_process_stop_t mode,
    char **out_error) {
    (void)context;
    posix_child_t *child = handle;
    /* A stop after the child was reaped is not an error, and must not signal:
     * the pid is no longer ours and the kernel is free to have reused it. */
    if (!child || child->pid <= 0 || child->reaped) return MAELYS_MCP_OK;
    int signal_number = mode == MAELYS_MCP_PROCESS_STOP_FORCED ?
        SIGKILL : SIGTERM;
    if (kill(child->pid, signal_number) != 0 && errno != ESRCH) {
        /* ESRCH is the child already being gone, which is exactly the
         * "a stop after exit is not an error" case. Anything else is a stop
         * that could not be delivered, and the ladder wants to hear about it:
         * it is the sharpest explanation available for the containment failure
         * that may be about to follow. */
        char text[128];
        (void)snprintf(text, sizeof(text),
            "cannot signal the child process (signal %d, errno %d)",
            signal_number, errno);
        set_posix_error(out_error, text);
        return MAELYS_MCP_ERR_IO;
    }
    return MAELYS_MCP_OK;
}

/* Local release only: no wait, no signal, nothing that can block. A child that
 * outlived the ladder is left behind deliberately, and the ladder is what
 * reports it. */
static void posix_release(void *context, void *handle) {
    (void)context;
    free(handle);
}

/*
 * .rodata, and shared by every POSIX launcher an embedder creates: the
 * contract borrows the ops table rather than copying it precisely so that this
 * is possible. abi_version is the macro and never a literal - a literal would
 * still be 5 the day the header moved to 6, which is the exact failure the
 * check exists to catch.
 */
static const maelys_mcp_process_ops_t posix_ops = {
    .abi_version = MAELYS_MCP_ABI_VERSION,
    .spawn = posix_spawn_child,
    .wait = posix_wait,
    .stop = posix_stop,
    .release = posix_release
};

maelys_mcp_result_t maelys_mcp_posix_launcher_create(
    maelys_mcp_process_launcher_t **out_launcher,
    char **out_error) {
    /*
     * A fresh reference, exactly like any other launcher. ABI 4's
     * process-lifetime singleton is gone deliberately: one lifetime rule that
     * always holds is easier to keep than one rule plus an exception that only
     * the stock launcher enjoys.
     *
     * No context and no release_context: everything this launcher needs is in
     * the handle it hands back per child.
     */
    return maelys_mcp_process_launcher_create("posix", &posix_ops, NULL, NULL,
        out_launcher, out_error);
}
