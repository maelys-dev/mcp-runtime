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
 * fork, the peer end on stdin and stdout, a scrubbed environment, execve.
 * It also implements MAELYS_MCP_PROCESS_FD_ISOLATED (native providers only -
 * see include/maelys/mcp/process_launcher.h): the peer end on fd 3 instead,
 * stdin on /dev/null, stdout wired to stderr, MAELYS_PROVIDER_FD=3 added to
 * the same scrubbed environment.
 *
 * This is the only file in the tree permitted to call fork, execve, socketpair,
 * waitpid or kill; scripts/audit_boundaries.sh enforces that. Everything above
 * the seam sees the four ops in src/process/launcher.h and no PID at all.
 */

/*
 * The launcher's handle. Opaque above the seam - the runtime stores it and
 * hands it back, and must never dereference it - and reaped is the load-bearing
 * field: once the child has been reaped its pid may be recycled by the kernel,
 * so a later stop must not signal it. The ladder calls stop after a wait that
 * may have reaped, which is exactly when that matters.
 */
typedef struct posix_child {
    pid_t pid;
    int reaped;
} posix_child_t;

/*
 * Where the protocol lands under MAELYS_MCP_PROCESS_FD_ISOLATED, and the
 * value MAELYS_PROVIDER_FD carries to the child so its SDK knows where to
 * look. The two must agree; docs/launch-contract-design.md fixes both at 3.
 */
#define MAELYS_MCP_ISOLATED_PROTOCOL_FD 3

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

/*
 * WNOHANG plus deadline polling, never a blocking waitpid: the bound is the
 * whole point of the ladder, and a child in uninterruptible sleep is unkillable
 * and would park teardown forever otherwise.
 */
static int wait_for_child(pid_t pid, unsigned int timeout_ms) {
    long long start = monotonic_milliseconds();
    if (start < 0) return 0;
    for (;;) {
        int status = 0;
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid || (waited < 0 && errno == ECHILD)) return 1;
        if (waited < 0 && errno != EINTR) return 0;
        long long now = monotonic_milliseconds();
        if (now < 0 || now - start >= (long long)timeout_ms) return 0;
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

static maelys_mcp_result_t posix_spawn_child(
    void *context,
    const maelys_mcp_process_spec_t *spec,
    maelys_mcp_process_instance_t *out_instance,
    char **out_error) {
    (void)context;
    if (!spec || !spec->executable_path || !spec->argv || !out_instance) {
        set_posix_error(out_error, "process spec is incomplete");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    if (!profile_is_accepted(spec->execution_profile)) {
        char text[256];
        (void)snprintf(text, sizeof(text),
            "execution profile \"%s\" is not supported by the posix launcher",
            spec->execution_profile);
        set_posix_error(out_error, text);
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    if (spec->fd_layout != MAELYS_MCP_PROCESS_FD_STDIO &&
        spec->fd_layout != MAELYS_MCP_PROCESS_FD_ISOLATED) {
        set_posix_error(out_error,
            "the posix launcher does not implement this descriptor layout");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        set_posix_error(out_error, "socketpair failed");
        return MAELYS_MCP_ERR_IO;
    }
    /* Both ends, so the parent's copy cannot leak into an unrelated child; the
     * child clears the flag again on the descriptors it keeps. */
    if (!set_close_on_exec(sockets[0]) || !set_close_on_exec(sockets[1])) {
        close(sockets[0]);
        close(sockets[1]);
        set_posix_error(out_error, "cannot protect the child's descriptors");
        return MAELYS_MCP_ERR_IO;
    }
    posix_child_t *child = calloc(1u, sizeof(*child));
    if (!child) {
        close(sockets[0]);
        close(sockets[1]);
        return MAELYS_MCP_ERR_MEMORY;
    }
    pid_t pid = fork();
    if (pid < 0) {
        free(child);
        close(sockets[0]);
        close(sockets[1]);
        set_posix_error(out_error, "fork failed");
        return MAELYS_MCP_ERR_IO;
    }
    if (pid == 0) {
        close(sockets[0]);
        if (spec->fd_layout == MAELYS_MCP_PROCESS_FD_ISOLATED) {
            /*
             * The protocol moves to fd 3 before fd 0/1/2 are touched at all:
             * if socketpair happened to hand back a low-numbered fd for the
             * child's end (0, 1 or 2), dup2'ing /dev/null or stderr onto that
             * number first would close the only copy of the socket this
             * child has, before it ever reaches fd 3.
             */
            if (dup2(sockets[1], MAELYS_MCP_ISOLATED_PROTOCOL_FD) < 0) _exit(126);
            if (sockets[1] != MAELYS_MCP_ISOLATED_PROTOCOL_FD) close(sockets[1]);
            /* Same oldfd == newfd hazard as the STDIO arrangement below,
             * applied to fd 3 instead of fd 0/1: dup2 is a no-op when
             * socketpair happened to hand back fd 3 itself, which would
             * leave FD_CLOEXEC set and make execve close the protocol end.
             * Cleared unconditionally, not only on the branch that believes a
             * duplication occurred. */
            if (fcntl(MAELYS_MCP_ISOLATED_PROTOCOL_FD, F_SETFD, 0) != 0) _exit(126);
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
            char *const isolated_environment[] = {
#ifdef __APPLE__
                (char *)"PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin",
#else
                (char *)"PATH=/usr/local/bin:/usr/bin:/bin",
#endif
                (char *)"LANG=C",
                (char *)"LC_ALL=C",
                (char *)"MAELYS_PROVIDER_FD=3",
                NULL
            };
            execve(spec->executable_path, spec->argv, isolated_environment);
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

        /* A closed allowlist, not a filtered inheritance: a child gets no
         * ambient credential from this process, and no caller-supplied or
         * request-supplied variable is ever added (docs/security-model.md). */
        char *const environment[] = {
#ifdef __APPLE__
            (char *)"PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin",
#else
            (char *)"PATH=/usr/local/bin:/usr/bin:/bin",
#endif
            (char *)"LANG=C",
            (char *)"LC_ALL=C",
            NULL
        };
        execve(spec->executable_path, spec->argv, environment);
        _exit(127);
    }
    close(sockets[1]);
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
        (void)wait_for_child(pid, spec->force_timeout_ms);
        free(child);
        set_posix_error(out_error, "cannot configure the child's socket safety");
        return MAELYS_MCP_ERR_IO;
    }
#endif
    out_instance->protocol_fd = sockets[0];
    out_instance->handle = child;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t posix_wait(
    void *context,
    void *handle,
    unsigned int timeout_ms,
    int *out_exited) {
    (void)context;
    if (!out_exited) return MAELYS_MCP_ERR_ARGUMENT;
    posix_child_t *child = handle;
    if (!child || child->pid <= 0 || child->reaped) {
        *out_exited = 1;
        return MAELYS_MCP_OK;
    }
    if (wait_for_child(child->pid, timeout_ms)) {
        child->reaped = 1;
        *out_exited = 1;
        return MAELYS_MCP_OK;
    }
    /* Not an error of this op's own: the budget expired and the child is still
     * there. The ladder decides what that means, and only the rung after a
     * forced stop makes it a containment failure. */
    *out_exited = 0;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t posix_stop(
    void *context,
    void *handle,
    maelys_mcp_process_stop_t mode) {
    (void)context;
    posix_child_t *child = handle;
    /* A stop after the child was reaped is not an error, and must not signal:
     * the pid is no longer ours and the kernel is free to have reused it. */
    if (!child || child->pid <= 0 || child->reaped) return MAELYS_MCP_OK;
    int signal_number = mode == MAELYS_MCP_PROCESS_STOP_FORCED ?
        SIGKILL : SIGTERM;
    if (kill(child->pid, signal_number) != 0 && errno != ESRCH) {
        return MAELYS_MCP_ERR_IO;
    }
    return MAELYS_MCP_OK;
}

/* Local release only: no wait, no signal, nothing that can block. A child that
 * outlived the ladder is left behind deliberately, and the ladder is what
 * reports it. */
static void posix_destroy(void *context, void *handle) {
    (void)context;
    free(handle);
}

static const maelys_mcp_process_ops_t posix_ops = {
    .spawn = posix_spawn_child,
    .wait = posix_wait,
    .stop = posix_stop,
    .destroy = posix_destroy
};

static const maelys_mcp_process_launcher_t posix_launcher = {
    .name = "posix",
    .ops = &posix_ops,
    .context = NULL
};

const maelys_mcp_process_launcher_t *maelys_mcp_posix_launcher(void) {
    return &posix_launcher;
}
