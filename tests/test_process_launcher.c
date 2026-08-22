/*
 * Conformance for the process launch seam (docs/launch-contract-design.md,
 * "ABI 5 - the launcher contract").
 *
 * Three things are being tested here, and the second and third are the reason
 * the file exists. The first is behaviour: spawn failures, a child that dies
 * before it describes, a crash with a call outstanding, the bounded shutdown
 * ladder and its escalation. The second is the *abstraction*: that the runtime
 * never treats the launcher's handle as a pointer worth checking for NULL,
 * never treats it as a pid, and survives a launcher that violates its side of
 * the contract. The third is what ABI 5 added: the refusal that guards the
 * vtable, the reference counting that replaced a written lifetime obligation,
 * the environment that now crosses the seam, and an exit status that can tell
 * a signal from a return.
 *
 * Every case runs against a fake launcher that starts no process at all - its
 * "children" are threads on the other end of a socketpair - and the cases that
 * can only be expressed with a real child run against the POSIX launcher too.
 * scripts/audit_boundaries.sh asserts that this file creates no process
 * itself: a fork here would mean the fake path was quietly testing the real
 * one.
 */

#include "maelys/mcp.h"

#include "src/internal/internal.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define ASSERT_TRUE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Assertion failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static long long milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (long long)now.tv_sec * 1000LL + (long long)now.tv_nsec / 1000000LL;
}

/* "No fd leaked" is a claim this suite makes repeatedly, so it is measured
 * rather than asserted: the delta across a case must be zero. */
static int open_descriptor_count(void) {
    int open_count = 0;
    for (int fd = 0; fd < 256; ++fd) {
        if (fcntl(fd, F_GETFD) >= 0) ++open_count;
    }
    return open_count;
}

/* --------------------------------------------------------------- the fake
 *
 * One launcher, several behaviours. It is deliberately not a mock of the POSIX
 * launcher: the point is to be a *different* substrate - threads and a
 * socketpair - so that anything the runtime silently assumed about processes
 * shows up as a failure here.
 */

typedef enum fake_behaviour {
    /* Speaks the maelys-provider protocol on the peer end. */
    FAKE_SERVE_NATIVE = 0,
    /* Speaks MCP on the peer end, for the mcp-proxy provider kind. */
    FAKE_SERVE_MCP,
    /* spawn returns non-OK. Nothing was started, nothing must be released. */
    FAKE_SPAWN_FAILS,
    /*
     * Takes the describe request off the wire and dies without answering it.
     * That is a child that died between spawn and its first response - the
     * ordinary shape of "the binary was not a provider at all". Dying one
     * moment earlier, before the request is even readable, is the same event
     * seen through a failed write rather than through EOF, which is why this
     * one reads first: it pins the code the runtime reports for a death,
     * rather than a race between two ways of noticing it.
     */
    FAKE_DIE_BEFORE_DESCRIBE,
    /* The transport stays open and nothing ever answers on it. */
    FAKE_SILENT,
    /* Speaks the native protocol until a call arrives, then dies holding it. */
    FAKE_DIE_MID_CALL,
    /* Same, on the MCP side. */
    FAKE_MCP_DIE_MID_CALL,
    /* spawn returns MAELYS_MCP_OK with a descriptor the runtime cannot use. */
    FAKE_INVALID_FD,
    /* Serves normally, but every wait reports that the child is still there -
     * a launcher that cannot terminate what it started. */
    FAKE_UNKILLABLE,
    /*
     * Unkillable, and honest about why: every stop fails with a message and
     * every wait fails with a different one. ABI 4 had nowhere to put either.
     */
    FAKE_DIAGNOSTIC_FAILURES
} fake_behaviour_t;

typedef enum fake_handle_style {
    /* The handle is a pointer to the launcher's per-instance state. */
    HANDLE_POINTER = 0,
    /* A stateless launcher: NULL is a legitimate handle, and must never be
     * read as "already released". */
    HANDLE_NULL,
    /* A handle that is not a pointer to anything at all. */
    HANDLE_OPAQUE
} fake_handle_style_t;

#define FAKE_OPAQUE_HANDLE ((void *)(uintptr_t)0x5eed)
#define FAKE_MAX_CHILDREN 4

/* The two messages the diagnostic launcher produces. The ladder must report
 * the first and discard the second, which is the whole of "keeps the best
 * diagnostic it has seen rather than letting a later, blander failure
 * overwrite an earlier, sharper one". */
#define FAKE_STOP_FAILURE "the fake launcher could not deliver the stop"
#define FAKE_WAIT_FAILURE "the fake launcher cannot tell whether it ended"

typedef struct fake_child {
    fake_behaviour_t behaviour;
    /*
     * The peer end. The runtime gets the other one and owns it from the
     * moment spawn returns OK. Guarded by mutex because the server thread
     * closes it when the child "dies" while stop() may be shutting it down:
     * closing and then reusing a descriptor number under another thread's
     * shutdown() is exactly the sort of bug a test must not have.
     */
    int peer_fd;
    pthread_t thread;
    int thread_started;
    pthread_mutex_t mutex;
    /* Set by stop(); the server thread returns when it sees it. GRACEFUL is
     * ignored by the escalation case, which is what forces the second rung. */
    int stop_requested;
    int ignores_graceful;
    /* Set by the server thread just before it returns. */
    int exited;
    int released;
} fake_child_t;

typedef struct fake_launcher {
    fake_behaviour_t behaviour;
    fake_handle_style_t handle_style;
    int ignores_graceful;
    const char *spawn_error;
    maelys_mcp_result_t spawn_status;

    pthread_mutex_t mutex;
    int spawn_count;
    int graceful_stops;
    int forced_stops;
    int wait_calls;
    int release_count;
    /* What the runtime handed back to the ops, recorded so a case can assert
     * the handle round-tripped untouched. */
    void *last_handle;
    int handle_seen_in_wait;
    int handle_seen_in_stop;
    int handle_seen_in_release;
    /* Incremented by the launcher's release_context, which the contract says
     * runs exactly once at zero, on whichever thread got there. */
    int context_releases;

    /*
     * What the runtime put in the REQUEST, read back through the public
     * getters and recorded here. Everything a case asserts about the request
     * is asserted about what actually crossed the seam, not about what the
     * runtime meant to send.
     */
    maelys_mcp_process_fd_layout_t last_layout;
    char last_executable[256];
    char last_arguments[512];
    size_t last_arg_count;
    int last_arg_past_end_was_null;
    char last_platform[64];
    char last_environment[512];
    size_t last_environment_count;
    int last_environment_past_end_wrote_nothing;
    int last_profile_absent;
    char last_profile[64];
    unsigned int last_spawn_timeout_ms;
    unsigned int last_grace_timeout_ms;
    unsigned int last_force_timeout_ms;

    fake_child_t *children[FAKE_MAX_CHILDREN];
    size_t child_count;
} fake_launcher_t;

static void fake_launcher_init(fake_launcher_t *fake, fake_behaviour_t behaviour) {
    memset(fake, 0, sizeof(*fake));
    fake->behaviour = behaviour;
    fake->spawn_status = MAELYS_MCP_ERR_IO;
    fake->spawn_error = "the fake launcher refused to spawn";
    (void)pthread_mutex_init(&fake->mutex, NULL);
}

/* Read under the mutex: a graceful stop clears this from the ladder's thread
 * while the server thread is deciding whether to outlive its transport. */
static int child_ignores_graceful(fake_child_t *child) {
    pthread_mutex_lock(&child->mutex);
    int ignores = child->ignores_graceful;
    pthread_mutex_unlock(&child->mutex);
    return ignores;
}

static int child_should_stop(fake_child_t *child) {
    pthread_mutex_lock(&child->mutex);
    int stop = child->stop_requested;
    pthread_mutex_unlock(&child->mutex);
    return stop;
}

static void child_request_stop(fake_child_t *child) {
    pthread_mutex_lock(&child->mutex);
    child->stop_requested = 1;
    child->ignores_graceful = 0;
    /* Unblocks a read without closing: stop must not block, and the
     * descriptor must stay valid until whoever owns it closes it. */
    if (child->peer_fd >= 0) (void)shutdown(child->peer_fd, SHUT_RDWR);
    pthread_mutex_unlock(&child->mutex);
}

/* Death, as the runtime is able to see it: the peer end goes away, so the
 * runtime's next read is EOF. */
static void child_die(fake_child_t *child) {
    pthread_mutex_lock(&child->mutex);
    int fd = child->peer_fd;
    child->peer_fd = -1;
    child->exited = 1;
    pthread_mutex_unlock(&child->mutex);
    if (fd >= 0) close(fd);
}

/*
 * The launcher's own bookkeeping, released by the test rather than by
 * release(): release is defined as local release only and must not join
 * anything it has not already observed exiting, and the containment case
 * deliberately never observes it. Somebody still has to reclaim the thread, so
 * the test does, after it has asserted what the runtime did.
 */
static void fake_launcher_clear(fake_launcher_t *fake) {
    for (size_t index = 0; index < fake->child_count; ++index) {
        fake_child_t *child = fake->children[index];
        if (!child) continue;
        child_request_stop(child);
        if (child->thread_started) (void)pthread_join(child->thread, NULL);
        child_die(child);
        pthread_mutex_destroy(&child->mutex);
        free(child);
        fake->children[index] = NULL;
    }
    fake->child_count = 0;
    pthread_mutex_destroy(&fake->mutex);
}

static json_t *native_response(json_t *id, json_t *result) {
    json_t *response = json_object();
    if (!response || !result) {
        if (response) json_decref(response);
        if (result) json_decref(result);
        return NULL;
    }
    if (json_object_set_new(response, "protocol",
            json_string(MAELYS_MCP_PROVIDER_PROTOCOL_FLOOR)) != 0 ||
        json_object_set(response, "id", id) != 0 ||
        json_object_set_new(response, "result", result) != 0) {
        json_decref(response);
        return NULL;
    }
    return response;
}

static json_t *native_describe_result(void) {
    return json_pack("{s:s,s:s,s:[{s:s,s:s,s:{s:s},s:s}]}",
        "name", "fake-provider",
        "version", "1",
        "tools",
            "name", "probe",
            "description", "a tool the fake provider answers",
            "inputSchema", "type", "object",
            "effect", "read");
}

/*
 * A child that has lost its transport is normally done - except when it is
 * playing the provider that ignores a polite request, which has to stay alive
 * past the closed socket so that the ladder is forced onto its second rung.
 * Returns non-zero when the loop should end.
 */
static int child_survives_eof(fake_child_t *child) {
    if (!child_ignores_graceful(child)) return 0;
    while (!child_should_stop(child)) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 5000000L};
        while (nanosleep(&pause, &pause) != 0) {}
    }
    return 0;
}

/* The in-process maelys-provider child. */
static void *native_server_main(void *opaque) {
    fake_child_t *child = opaque;
    maelys_mcp_line_reader_t reader;
    if (maelys_mcp_line_reader_init(&reader, 65536u) != MAELYS_MCP_OK) {
        child_die(child);
        return NULL;
    }
    while (!child_should_stop(child)) {
        json_t *message = NULL;
        char *error = NULL;
        if (maelys_mcp_line_reader_read(&reader, child->peer_fd, &message,
            &error) != MAELYS_MCP_OK) {
            free(error);
            (void)child_survives_eof(child);
            break;
        }
        json_t *id = json_object_get(message, "id");
        const char *method = json_string_value(json_object_get(message, "method"));
        json_t *result = NULL;
        int finished = 0;
        if (method && strcmp(method, "provider/describe") == 0) {
            if (child->behaviour == FAKE_DIE_BEFORE_DESCRIBE) {
                json_decref(message);
                break;
            }
            result = native_describe_result();
        } else if (method && strcmp(method, "provider/call") == 0) {
            if (child->behaviour == FAKE_DIE_MID_CALL) {
                /* Dies holding the call: the caller must wake on the latched
                 * failure rather than at its own deadline. */
                json_decref(message);
                break;
            }
            result = json_pack("{s:s,s:[{s:s,s:s}]}",
                "resultType", "complete",
                "content", "type", "text", "text", "ok");
        } else if (method && strcmp(method, "provider/shutdown") == 0) {
            if (child_ignores_graceful(child)) {
                /* Deaf to every polite request, protocol and signal alike -
                 * the same shape as the stubborn fixture the POSIX half of
                 * this case uses, and the only way the ladder's second rung is
                 * ever reached. */
                json_decref(message);
                continue;
            }
            result = json_object();
            finished = 1;
        } else {
            result = json_object();
        }
        json_t *response = native_response(id, result);
        if (response) {
            (void)maelys_mcp_write_json_line_with_timeout(child->peer_fd,
                response, 2000u);
            json_decref(response);
        }
        json_decref(message);
        if (finished) break;
    }
    maelys_mcp_line_reader_clear(&reader);
    child_die(child);
    return NULL;
}

static json_t *mcp_response(json_t *id, json_t *result) {
    json_t *response = json_object();
    if (!response || !result) {
        if (response) json_decref(response);
        if (result) json_decref(result);
        return NULL;
    }
    if (json_object_set_new(response, "jsonrpc", json_string("2.0")) != 0 ||
        json_object_set(response, "id", id) != 0 ||
        json_object_set_new(response, "result", result) != 0) {
        json_decref(response);
        return NULL;
    }
    return response;
}

/* The in-process third-party MCP server. Modern era: it advertises the dated
 * revision the proxy asks about, so no legacy handshake is needed. */
static void *mcp_server_main(void *opaque) {
    fake_child_t *child = opaque;
    maelys_mcp_line_reader_t reader;
    if (maelys_mcp_line_reader_init(&reader, 65536u) != MAELYS_MCP_OK) {
        child_die(child);
        return NULL;
    }
    while (!child_should_stop(child)) {
        json_t *message = NULL;
        char *error = NULL;
        if (maelys_mcp_line_reader_read(&reader, child->peer_fd, &message,
            &error) != MAELYS_MCP_OK) {
            free(error);
            (void)child_survives_eof(child);
            break;
        }
        json_t *id = json_object_get(message, "id");
        const char *method = json_string_value(json_object_get(message, "method"));
        if (!id || json_is_null(id)) {
            json_decref(message);
            continue;
        }
        json_t *result = NULL;
        if (method && strcmp(method, "server/discover") == 0) {
            result = json_pack("{s:[s]}", "supportedVersions",
                MAELYS_MCP_PROTOCOL_MODERN);
        } else if (method && strcmp(method, "tools/list") == 0) {
            result = json_pack("{s:[{s:s,s:s,s:{s:s}}]}",
                "tools",
                    "name", "probe",
                    "description", "a tool the fake upstream answers",
                    "inputSchema", "type", "object");
        } else if (method && strcmp(method, "tools/call") == 0) {
            if (child->behaviour == FAKE_MCP_DIE_MID_CALL) {
                json_decref(message);
                break;
            }
            result = json_pack("{s:[{s:s,s:s}]}",
                "content", "type", "text", "text", "ok");
        } else {
            result = json_object();
        }
        json_t *response = mcp_response(id, result);
        if (response) {
            (void)maelys_mcp_write_json_line_with_timeout(child->peer_fd,
                response, 2000u);
            json_decref(response);
        }
        json_decref(message);
    }
    maelys_mcp_line_reader_clear(&reader);
    child_die(child);
    return NULL;
}

/*
 * Where the launcher keeps what it started. The handle style decides what the
 * *runtime* is given: a pointer to this, NULL, or a small integer - and the
 * runtime has to work identically in all three, which is the whole point of
 * the NULL-handle and opacity cases.
 */
static fake_child_t *resolve_child(fake_launcher_t *fake, void *handle) {
    if (fake->handle_style == HANDLE_POINTER) return handle;
    return fake->child_count ? fake->children[0] : NULL;
}

static void *handle_for(fake_launcher_t *fake, fake_child_t *child) {
    switch (fake->handle_style) {
        case HANDLE_NULL: return NULL;
        case HANDLE_OPAQUE: return FAKE_OPAQUE_HANDLE;
        case HANDLE_POINTER: break;
    }
    return child;
}

/*
 * The request, read the way a real launcher reads one: through the getters and
 * through nothing else. Recorded into the fake so a case can assert what
 * crossed the seam.
 *
 * The two "past the end" probes are here because the contract makes promises
 * about out-of-range indices, and a launcher walking the vectors is exactly
 * what would discover they were wrong.
 */
static void record_request(
    fake_launcher_t *fake,
    const maelys_mcp_process_request_t *request) {
    fake->last_layout = maelys_mcp_process_request_fd_layout(request);
    (void)snprintf(fake->last_executable, sizeof(fake->last_executable), "%s",
        maelys_mcp_process_request_executable(request));
    const char *profile = maelys_mcp_process_request_execution_profile(request);
    fake->last_profile_absent = profile == NULL;
    (void)snprintf(fake->last_profile, sizeof(fake->last_profile), "%s",
        profile ? profile : "");
    fake->last_spawn_timeout_ms =
        maelys_mcp_process_request_spawn_timeout_ms(request);
    fake->last_grace_timeout_ms =
        maelys_mcp_process_request_grace_timeout_ms(request);
    fake->last_force_timeout_ms =
        maelys_mcp_process_request_force_timeout_ms(request);

    size_t arg_count = maelys_mcp_process_request_arg_count(request);
    fake->last_arg_count = arg_count;
    fake->last_arguments[0] = '\0';
    size_t used = 0u;
    for (size_t index = 0; index < arg_count; ++index) {
        const char *argument = maelys_mcp_process_request_arg_at(request, index);
        int written = snprintf(fake->last_arguments + used,
            sizeof(fake->last_arguments) - used, "%s%s",
            index ? "|" : "", argument ? argument : "(null)");
        if (written < 0 ||
            (size_t)written >= sizeof(fake->last_arguments) - used) break;
        used += (size_t)written;
    }
    fake->last_arg_past_end_was_null =
        maelys_mcp_process_request_arg_at(request, arg_count) == NULL;

    (void)snprintf(fake->last_platform, sizeof(fake->last_platform), "%s",
        maelys_mcp_process_request_environment_platform(request));
    size_t environment_count =
        maelys_mcp_process_request_environment_count(request);
    fake->last_environment_count = environment_count;
    fake->last_environment[0] = '\0';
    used = 0u;
    for (size_t index = 0; index < environment_count; ++index) {
        const char *name = NULL;
        const char *value = NULL;
        if (!maelys_mcp_process_request_environment_at(request, index, &name,
            &value)) break;
        int written = snprintf(fake->last_environment + used,
            sizeof(fake->last_environment) - used, "%s%s=%s",
            index ? "|" : "", name, value);
        if (written < 0 ||
            (size_t)written >= sizeof(fake->last_environment) - used) break;
        used += (size_t)written;
    }
    /* Seeded with a value the getter must not overwrite, so "wrote neither" is
     * checked rather than assumed. */
    const char *past_name = (const char *)fake;
    const char *past_value = (const char *)fake;
    fake->last_environment_past_end_wrote_nothing =
        maelys_mcp_process_request_environment_at(request, environment_count,
            &past_name, &past_value) == 0 &&
        past_name == (const char *)fake && past_value == (const char *)fake;
}

static maelys_mcp_result_t fake_spawn(
    void *context,
    const maelys_mcp_process_request_t *request,
    void **out_handle,
    int *out_protocol_fd,
    char **out_error) {
    fake_launcher_t *fake = context;
    pthread_mutex_lock(&fake->mutex);
    ++fake->spawn_count;
    record_request(fake, request);
    pthread_mutex_unlock(&fake->mutex);

    if (fake->behaviour == FAKE_SPAWN_FAILS) {
        if (out_error) {
            free(*out_error);
            *out_error = maelys_mcp_strdup(fake->spawn_error);
        }
        /* Neither out parameter is written on a failed spawn, so a runtime
         * that read one would be reading what it initialized. */
        return fake->spawn_status;
    }
    if (fake->behaviour == FAKE_INVALID_FD) {
        /* A contract violation the runtime is the only side able to detect:
         * this launcher believes it succeeded. */
        *out_protocol_fd = -1;
        *out_handle = handle_for(fake, NULL);
        return MAELYS_MCP_OK;
    }
    if (fake->child_count >= FAKE_MAX_CHILDREN) return MAELYS_MCP_ERR_STATE;
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return MAELYS_MCP_ERR_IO;
    fake_child_t *child = calloc(1u, sizeof(*child));
    if (!child) {
        close(sockets[0]);
        close(sockets[1]);
        return MAELYS_MCP_ERR_MEMORY;
    }
    child->behaviour = fake->behaviour;
    child->peer_fd = sockets[1];
    child->ignores_graceful = fake->ignores_graceful;
    (void)pthread_mutex_init(&child->mutex, NULL);
    fake->children[fake->child_count++] = child;

    if (fake->behaviour != FAKE_SILENT) {
        void *(*server)(void *) =
            (fake->behaviour == FAKE_SERVE_MCP ||
             fake->behaviour == FAKE_MCP_DIE_MID_CALL) ?
                mcp_server_main : native_server_main;
        if (pthread_create(&child->thread, NULL, server, child) != 0) {
            close(sockets[0]);
            return MAELYS_MCP_ERR_IO;
        }
        child->thread_started = 1;
    }
    *out_protocol_fd = sockets[0];
    *out_handle = handle_for(fake, child);
    return MAELYS_MCP_OK;
}

static void fake_set_error(char **out_error, const char *message) {
    if (!out_error) return;
    free(*out_error);
    *out_error = maelys_mcp_strdup(message);
}

static maelys_mcp_result_t fake_wait(
    void *context,
    void *handle,
    unsigned int timeout_ms,
    maelys_mcp_process_exit_status_t *out_status,
    char **out_error) {
    fake_launcher_t *fake = context;
    pthread_mutex_lock(&fake->mutex);
    ++fake->wait_calls;
    fake->last_handle = handle;
    fake->handle_seen_in_wait = 1;
    pthread_mutex_unlock(&fake->mutex);
    out_status->exited = 0;
    out_status->exit_code = 0;
    out_status->term_signal = 0;
    if (fake->behaviour == FAKE_DIAGNOSTIC_FAILURES) {
        fake_set_error(out_error, FAKE_WAIT_FAILURE);
        return MAELYS_MCP_ERR_IO;
    }
    fake_child_t *child = resolve_child(fake, handle);
    if (!child) {
        out_status->exited = 1;
        return MAELYS_MCP_OK;
    }
    if (fake->behaviour == FAKE_UNKILLABLE) {
        /* The honest shape of a containment failure: the budget is spent and
         * the child is still there. Not an error of the op's own - the
         * launcher answered the question, and the answer was no. */
        long long deadline = milliseconds() + (long long)timeout_ms;
        while (milliseconds() < deadline) {
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 5000000L};
            while (nanosleep(&pause, &pause) != 0) {}
        }
        return MAELYS_MCP_OK;
    }
    long long deadline = milliseconds() + (long long)timeout_ms;
    for (;;) {
        pthread_mutex_lock(&child->mutex);
        int exited = child->exited;
        pthread_mutex_unlock(&child->mutex);
        if (exited) {
            out_status->exited = 1;
            return MAELYS_MCP_OK;
        }
        if (milliseconds() >= deadline) return MAELYS_MCP_OK;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 5000000L};
        while (nanosleep(&pause, &pause) != 0) {}
    }
}

static maelys_mcp_result_t fake_stop(
    void *context,
    void *handle,
    maelys_mcp_process_stop_t mode,
    char **out_error) {
    fake_launcher_t *fake = context;
    pthread_mutex_lock(&fake->mutex);
    if (mode == MAELYS_MCP_PROCESS_STOP_FORCED) ++fake->forced_stops;
    else ++fake->graceful_stops;
    fake->last_handle = handle;
    fake->handle_seen_in_stop = 1;
    pthread_mutex_unlock(&fake->mutex);
    if (fake->behaviour == FAKE_DIAGNOSTIC_FAILURES) {
        fake_set_error(out_error, FAKE_STOP_FAILURE);
        return MAELYS_MCP_ERR_IO;
    }
    fake_child_t *child = resolve_child(fake, handle);
    if (!child) return MAELYS_MCP_OK;
    if (mode == MAELYS_MCP_PROCESS_STOP_GRACEFUL &&
        child_ignores_graceful(child)) {
        /* A child that ignores the polite request, which is the only reason
         * the second rung exists. */
        return MAELYS_MCP_OK;
    }
    child_request_stop(child);
    /* A child with nothing running behind it has, by construction, already
     * terminated - there is nothing for a wait to observe otherwise. */
    if (!child->thread_started) child_die(child);
    return MAELYS_MCP_OK;
}

static void fake_release(void *context, void *handle) {
    fake_launcher_t *fake = context;
    pthread_mutex_lock(&fake->mutex);
    ++fake->release_count;
    fake->last_handle = handle;
    fake->handle_seen_in_release = 1;
    pthread_mutex_unlock(&fake->mutex);
    fake_child_t *child = resolve_child(fake, handle);
    /* Local release only. The thread and its descriptor are the launcher's own
     * bookkeeping and are reclaimed in fake_launcher_clear, because release is
     * not allowed to wait for anything. */
    if (child) child->released = 1;
}

/*
 * .rodata, shared by every fake launcher this file creates - which is the
 * property "ops is borrowed rather than copied" exists to allow, and this is
 * the only place in the tree that exercises it with more than one launcher.
 */
static const maelys_mcp_process_ops_t fake_ops = {
    .abi_version = MAELYS_MCP_ABI_VERSION,
    .spawn = fake_spawn,
    .wait = fake_wait,
    .stop = fake_stop,
    .release = fake_release
};

/* The releaser a launcher over a fake carries when a case needs to observe the
 * last reference going away. Locked, because the thread that gets here is
 * whichever teardown finished last. */
static void fake_context_release(void *context) {
    fake_launcher_t *fake = context;
    pthread_mutex_lock(&fake->mutex);
    ++fake->context_releases;
    pthread_mutex_unlock(&fake->mutex);
}

static maelys_mcp_process_launcher_t *fake_launcher_create(fake_launcher_t *fake) {
    maelys_mcp_process_launcher_t *launcher = NULL;
    char *error = NULL;
    if (maelys_mcp_process_launcher_create("fake", &fake_ops, fake, NULL,
        &launcher, &error) != MAELYS_MCP_OK) {
        free(error);
        return NULL;
    }
    free(error);
    return launcher;
}

/* ------------------------------------------------------------ shared drivers */

static maelys_mcp_result_t call_directly(
    maelys_mcp_provider_t *provider,
    const char *tool_name,
    char **out_error) {
    json_t *arguments = json_object();
    if (!arguments) return MAELYS_MCP_ERR_MEMORY;
    maelys_mcp_provider_request_t request = {
        .tool_name = tool_name, .arguments = arguments
    };
    maelys_mcp_provider_result_t result;
    maelys_mcp_provider_result_init(&result);
    maelys_mcp_result_t status = provider->call(
        provider->context, &request, &result, out_error);
    maelys_mcp_provider_result_clear(&result);
    json_decref(arguments);
    return status;
}

/* The example provider's echo tool, which requires its argument - so this is a
 * call that reaches the child and comes back with a result, rather than one
 * the schema check answers before the transport is ever used. */
static maelys_mcp_result_t call_echo(
    maelys_mcp_provider_t *provider,
    char **out_error) {
    json_t *arguments = json_pack("{s:s}", "message", "still here");
    if (!arguments) return MAELYS_MCP_ERR_MEMORY;
    maelys_mcp_provider_request_t request = {
        .tool_name = "example.echo", .arguments = arguments
    };
    maelys_mcp_provider_result_t result;
    maelys_mcp_provider_result_init(&result);
    maelys_mcp_result_t status = provider->call(
        provider->context, &request, &result, out_error);
    maelys_mcp_provider_result_clear(&result);
    json_decref(arguments);
    return status;
}

static maelys_mcp_provider_process_options_t native_options(
    unsigned int describe_timeout_ms,
    unsigned int shutdown_timeout_ms) {
    maelys_mcp_provider_process_options_t options = {
        /* Absolute, and never reached: the fake launcher starts a thread. */
        .executable_path = "/fake/provider",
        .max_message_bytes = 65536u,
        .describe_timeout_ms = describe_timeout_ms,
        .call_timeout_ms = 10000u,
        .shutdown_timeout_ms = shutdown_timeout_ms
    };
    return options;
}

static maelys_mcp_proxy_options_t proxy_options(unsigned int connect_timeout_ms) {
    maelys_mcp_proxy_options_t options = {
        .executable_path = "/fake/upstream",
        .max_message_bytes = 65536u,
        .connect_timeout_ms = connect_timeout_ms,
        .call_timeout_ms = 10000u,
        .default_effect = MAELYS_MCP_EFFECT_READ
    };
    return options;
}

/* The internal launch parameters, for the cases that drive the seam directly
 * rather than through a provider. */
static maelys_mcp_process_launch_params_t launch_params(
    const char *executable,
    char *const *argv,
    maelys_mcp_process_fd_layout_t fd_layout) {
    maelys_mcp_process_launch_params_t params = {
        .executable_path = executable,
        .argv = argv,
        .execution_profile = NULL,
        .fd_layout = fd_layout,
        .spawn_timeout_ms = 5000u,
        .grace_timeout_ms = 500u,
        .force_timeout_ms = 500u
    };
    return params;
}

static maelys_mcp_process_slot_t empty_slot(void) {
    maelys_mcp_process_slot_t slot = {
        .launcher = NULL, .handle = NULL, .protocol_fd = -1, .live = 0
    };
    return slot;
}

/* ------------------------------------------------------------------ the cases */

/* 1. Spawn failure. No fd, no child, no release, and the launcher's own error
 * reaches the caller verbatim rather than being replaced by a generic one.
 *
 * The POSIX equivalent - execve failing on a path that is not a provider -
 * cannot fail the *spawn*: the fork succeeds and the failure surfaces as the
 * child dying before it describes, which is case 2, and case 2 does run
 * against the POSIX launcher. */
static int case_spawn_failure(void) {
    for (int kind = 0; kind < 2; ++kind) {
        int before = open_descriptor_count();
        fake_launcher_t fake;
        fake_launcher_init(&fake, FAKE_SPAWN_FAILS);
        maelys_mcp_process_launcher_t *launcher = fake_launcher_create(&fake);
        ASSERT_TRUE(launcher != NULL);
        maelys_mcp_provider_t *provider = NULL;
        char *error = NULL;
        maelys_mcp_result_t status;
        if (kind == 0) {
            maelys_mcp_provider_process_options_t options = native_options(1000u, 200u);
            status = maelys_mcp_provider_spawn_with_launcher(&options, launcher,
                &provider, &error);
        } else {
            maelys_mcp_proxy_options_t options = proxy_options(1000u);
            status = maelys_mcp_provider_proxy_spawn_with_launcher(&options,
                launcher, &provider, NULL, &error);
        }
        ASSERT_TRUE(status == MAELYS_MCP_ERR_IO);
        ASSERT_TRUE(provider == NULL);
        ASSERT_TRUE(error != NULL);
        ASSERT_TRUE(strcmp(error, "the fake launcher refused to spawn") == 0);
        ASSERT_TRUE(fake.spawn_count == 1);
        ASSERT_TRUE(fake.release_count == 0);
        free(error);
        maelys_mcp_process_launcher_release(launcher);
        fake_launcher_clear(&fake);
        ASSERT_TRUE(open_descriptor_count() == before);
    }
    return 0;
}

/* 2. Death before describe. The failure is MAELYS_MCP_ERR_PROVIDER, it arrives
 * well before the describe deadline rather than at it, and teardown is clean. */
static int case_death_before_describe(const char *dead_executable) {
    int before = open_descriptor_count();
    fake_launcher_t fake;
    fake_launcher_init(&fake, FAKE_DIE_BEFORE_DESCRIBE);
    maelys_mcp_process_launcher_t *launcher = fake_launcher_create(&fake);
    ASSERT_TRUE(launcher != NULL);
    maelys_mcp_provider_process_options_t options = native_options(10000u, 200u);
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    long long started = milliseconds();
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options, launcher,
        &provider, &error) == MAELYS_MCP_ERR_PROVIDER);
    ASSERT_TRUE(milliseconds() - started < 5000LL);
    ASSERT_TRUE(provider == NULL);
    ASSERT_TRUE(error != NULL);
    /* The failed spawn still tore the instance down exactly once. */
    ASSERT_TRUE(fake.release_count == 1);
    free(error);
    maelys_mcp_process_launcher_release(launcher);
    fake_launcher_clear(&fake);
    ASSERT_TRUE(open_descriptor_count() == before);

    /*
     * The same case with a real child, through the launcher that forks. The
     * describe deadline is deliberately enormous and is not what this case
     * measures: it is the bound the failure has to beat, and a fresh binary's
     * first exec on a loaded machine has been slow enough to matter - the same
     * reason tests/test_process_provider.c's fixture spawns use 30s.
     */
    before = open_descriptor_count();
    maelys_mcp_provider_process_options_t posix_options =
        native_options(30000u, 500u);
    posix_options.executable_path = dead_executable;
    provider = NULL;
    error = NULL;
    started = milliseconds();
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_options(&posix_options, &provider,
        &error) != MAELYS_MCP_OK);
    ASSERT_TRUE(milliseconds() - started < 20000LL);
    ASSERT_TRUE(provider == NULL);
    free(error);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/* 3. Describe timeout. It fails at the deadline, not later, and the ladder
 * still runs on a child that never said anything. */
static int case_describe_timeout(const char *silent_executable) {
    int before = open_descriptor_count();
    fake_launcher_t fake;
    fake_launcher_init(&fake, FAKE_SILENT);
    maelys_mcp_process_launcher_t *launcher = fake_launcher_create(&fake);
    ASSERT_TRUE(launcher != NULL);
    maelys_mcp_provider_process_options_t options = native_options(300u, 200u);
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    long long started = milliseconds();
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options, launcher,
        &provider, &error) != MAELYS_MCP_OK);
    long long elapsed = milliseconds() - started;
    ASSERT_TRUE(elapsed >= 300LL);
    ASSERT_TRUE(elapsed < 5000LL);
    ASSERT_TRUE(provider == NULL);
    ASSERT_TRUE(fake.release_count == 1);
    free(error);
    maelys_mcp_process_launcher_release(launcher);
    fake_launcher_clear(&fake);
    ASSERT_TRUE(open_descriptor_count() == before);

    /* A real child that takes a second to answer, against a deadline of a
     * fifth of that. */
    before = open_descriptor_count();
    maelys_mcp_provider_process_options_t posix_options = native_options(200u, 500u);
    posix_options.executable_path = silent_executable;
    provider = NULL;
    error = NULL;
    started = milliseconds();
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_options(&posix_options, &provider,
        &error) != MAELYS_MCP_OK);
    ASSERT_TRUE(milliseconds() - started >= 200LL);
    ASSERT_TRUE(provider == NULL);
    free(error);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/*
 * 4. Crash mid-call, for BOTH provider kinds through the seam. The blocked
 * caller must wake on the latched failure rather than at its own deadline -
 * the call timeout here is ten seconds and the assertion is two - and both
 * kinds must report the same result code and the same shape of message.
 */
static int case_crash_mid_call(void) {
    maelys_mcp_result_t codes[2];
    char *messages[2] = {NULL, NULL};
    for (int kind = 0; kind < 2; ++kind) {
        int before = open_descriptor_count();
        fake_launcher_t fake;
        fake_launcher_init(&fake,
            kind == 0 ? FAKE_DIE_MID_CALL : FAKE_MCP_DIE_MID_CALL);
        maelys_mcp_process_launcher_t *launcher = fake_launcher_create(&fake);
        ASSERT_TRUE(launcher != NULL);
        maelys_mcp_provider_t *provider = NULL;
        char *error = NULL;
        if (kind == 0) {
            maelys_mcp_provider_process_options_t options = native_options(5000u, 200u);
            ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options,
                launcher, &provider, &error) == MAELYS_MCP_OK);
        } else {
            maelys_mcp_proxy_options_t options = proxy_options(5000u);
            ASSERT_TRUE(maelys_mcp_provider_proxy_spawn_with_launcher(&options,
                launcher, &provider, NULL, &error) == MAELYS_MCP_OK);
        }
        ASSERT_TRUE(provider != NULL);
        free(error);
        error = NULL;
        long long started = milliseconds();
        codes[kind] = call_directly(provider, "probe", &error);
        ASSERT_TRUE(codes[kind] != MAELYS_MCP_OK);
        ASSERT_TRUE(milliseconds() - started < 2000LL);
        ASSERT_TRUE(error != NULL && *error != '\0');
        messages[kind] = error;
        maelys_mcp_provider_destroy(provider);
        ASSERT_TRUE(fake.release_count == 1);
        maelys_mcp_process_launcher_release(launcher);
        fake_launcher_clear(&fake);
        ASSERT_TRUE(open_descriptor_count() == before);
    }
    /* One death, one verdict: a provider that dies under a call is
     * MAELYS_MCP_ERR_PROVIDER whichever kind it was. */
    ASSERT_TRUE(codes[0] == MAELYS_MCP_ERR_PROVIDER);
    ASSERT_TRUE(codes[1] == MAELYS_MCP_ERR_PROVIDER);
    free(messages[0]);
    free(messages[1]);
    return 0;
}

/* 5. Graceful stop. A child that exits when asked is never forced, which is
 * asserted by counting rather than by inspection. */
static int case_graceful_stop(void) {
    int before = open_descriptor_count();
    fake_launcher_t fake;
    fake_launcher_init(&fake, FAKE_SERVE_NATIVE);
    maelys_mcp_process_launcher_t *launcher = fake_launcher_create(&fake);
    ASSERT_TRUE(launcher != NULL);
    maelys_mcp_provider_process_options_t options = native_options(5000u, 1000u);
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options, launcher,
        &provider, &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    ASSERT_TRUE(call_directly(provider, "probe", &error) == MAELYS_MCP_OK);
    free(error);
    maelys_mcp_provider_destroy(provider);
    ASSERT_TRUE(fake.graceful_stops == 1);
    ASSERT_TRUE(fake.forced_stops == 0);
    ASSERT_TRUE(fake.release_count == 1);
    maelys_mcp_process_launcher_release(launcher);
    fake_launcher_clear(&fake);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/*
 * 6. Forced escalation. A child that ignores the polite request is killed
 * after the grace budget and teardown still completes - the property the
 * blocking waitpid this ladder replaced could not offer.
 */
static int case_forced_escalation(const char *stubborn_executable) {
    int before = open_descriptor_count();
    fake_launcher_t fake;
    fake_launcher_init(&fake, FAKE_SERVE_NATIVE);
    fake.ignores_graceful = 1;
    maelys_mcp_process_launcher_t *launcher = fake_launcher_create(&fake);
    ASSERT_TRUE(launcher != NULL);
    maelys_mcp_provider_process_options_t options = native_options(5000u, 300u);
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options, launcher,
        &provider, &error) == MAELYS_MCP_OK);
    free(error);
    long long started = milliseconds();
    maelys_mcp_provider_destroy(provider);
    long long elapsed = milliseconds() - started;
    ASSERT_TRUE(fake.graceful_stops == 1);
    ASSERT_TRUE(fake.forced_stops == 1);
    ASSERT_TRUE(fake.release_count == 1);
    /* Bounded: the grace budget was spent, and nothing beyond the force
     * budget was. */
    ASSERT_TRUE(elapsed >= 300LL);
    ASSERT_TRUE(elapsed < 10000LL);
    maelys_mcp_process_launcher_release(launcher);
    fake_launcher_clear(&fake);
    ASSERT_TRUE(open_descriptor_count() == before);

    /*
     * The same escalation with a real child: the stubborn fixture answers
     * describe, then ignores both the provider/shutdown request and SIGTERM,
     * so only SIGKILL ends it. The describe deadline is a liveness bound this
     * case does not measure and gets the loose 30s the other fixture spawns in
     * this repository use; what is measured is that teardown returns at all.
     */
    before = open_descriptor_count();
    maelys_mcp_provider_process_options_t posix_options = native_options(30000u, 300u);
    posix_options.executable_path = stubborn_executable;
    provider = NULL;
    error = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_options(&posix_options, &provider,
        &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    started = milliseconds();
    maelys_mcp_provider_destroy(provider);
    ASSERT_TRUE(milliseconds() - started < 20000LL);
    ASSERT_TRUE(open_descriptor_count() == before);

    /*
     * That teardown returned proves the ladder is bounded; it does not prove
     * the child was actually contained, because a ladder that gave up would
     * also return - it would just leak, which no caller above the seam can
     * see. So the same fixture is driven through the seam directly, where the
     * ladder's own verdict is visible: MAELYS_MCP_OK means the child was
     * observed to exit, and only a genuine SIGKILL can produce that against a
     * process ignoring SIGTERM.
     */
    before = open_descriptor_count();
    maelys_mcp_process_launcher_t *posix_launcher = NULL;
    ASSERT_TRUE(maelys_mcp_posix_launcher_create(&posix_launcher, &error) ==
        MAELYS_MCP_OK);
    free(error);
    error = NULL;
    char *const stubborn_argv[] = {
        (char *)stubborn_executable, (char *)"--provider", NULL
    };
    maelys_mcp_process_launch_params_t params = launch_params(
        stubborn_executable, stubborn_argv, MAELYS_MCP_PROCESS_FD_STDIO);
    params.grace_timeout_ms = 300u;
    params.force_timeout_ms = 2000u;
    maelys_mcp_process_slot_t slot = empty_slot();
    ASSERT_TRUE(maelys_mcp_process_launch(posix_launcher, &params, &slot,
        &error) == MAELYS_MCP_OK);
    /* The fixture ignores SIGTERM only once it has been asked to shut down, so
     * the request has to reach it first - and it never answers. */
    json_t *goodbye = json_pack("{s:s,s:i,s:s,s:{}}",
        "protocol", MAELYS_MCP_PROVIDER_PROTOCOL_FLOOR,
        "id", 1,
        "method", "provider/shutdown",
        "params");
    ASSERT_TRUE(goodbye != NULL);
    ASSERT_TRUE(maelys_mcp_write_json_line(slot.protocol_fd, goodbye) ==
        MAELYS_MCP_OK);
    json_decref(goodbye);
    struct timespec settle = {.tv_sec = 0, .tv_nsec = 200000000L};
    while (nanosleep(&settle, &settle) != 0) {}
    close(slot.protocol_fd);
    started = milliseconds();
    ASSERT_TRUE(maelys_mcp_process_shutdown(&slot, 300u, 2000u, &error) ==
        MAELYS_MCP_OK);
    /* Past the graceful budget, because the polite request was ignored, and
     * nowhere near the forced one, because the kill landed. */
    elapsed = milliseconds() - started;
    ASSERT_TRUE(elapsed >= 300LL);
    ASSERT_TRUE(elapsed < 2300LL);
    free(error);
    maelys_mcp_process_launcher_release(posix_launcher);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/*
 * 7. Containment failure - the case that proves the ladder is bounded at its
 * LAST rung and not merely at its middle ones. A launcher whose every wait
 * reports the child still running gets both budgets spent, release called
 * exactly once anyway, and MAELYS_MCP_ERR_STATE with a diagnostic naming it.
 * The contract chooses the leak over the hang, and requires it to be reported.
 */
static int case_containment_failure(void) {
    int before = open_descriptor_count();
    fake_launcher_t fake;
    fake_launcher_init(&fake, FAKE_UNKILLABLE);
    maelys_mcp_process_launcher_t *launcher = fake_launcher_create(&fake);
    ASSERT_TRUE(launcher != NULL);
    char *const argv[] = {(char *)"/fake/provider", NULL};
    maelys_mcp_process_launch_params_t params = launch_params("/fake/provider",
        argv, MAELYS_MCP_PROCESS_FD_STDIO);
    params.grace_timeout_ms = 100u;
    params.force_timeout_ms = 100u;
    maelys_mcp_process_slot_t slot = empty_slot();
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_process_launch(launcher, &params, &slot, &error) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(slot.live == 1);
    close(slot.protocol_fd);
    long long started = milliseconds();
    maelys_mcp_result_t status = maelys_mcp_process_shutdown(&slot, 100u, 100u,
        &error);
    long long elapsed = milliseconds() - started;
    ASSERT_TRUE(status == MAELYS_MCP_ERR_STATE);
    ASSERT_TRUE(error != NULL && strstr(error, "fake") != NULL);
    ASSERT_TRUE(fake.graceful_stops == 1);
    ASSERT_TRUE(fake.forced_stops == 1);
    ASSERT_TRUE(fake.wait_calls == 2);
    ASSERT_TRUE(fake.release_count == 1);
    ASSERT_TRUE(slot.live == 0);
    /* Roughly grace + force, and emphatically not forever. */
    ASSERT_TRUE(elapsed >= 200LL);
    ASSERT_TRUE(elapsed < 10000LL);
    free(error);
    maelys_mcp_process_launcher_release(launcher);
    fake_launcher_clear(&fake);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/*
 * 8. Double teardown. The ladder is driven twice and release still reaches the
 * launcher once, because exactly-once is tracked by the runtime's own private
 * `live` flag rather than by anything about the handle. ABI 4 tracked the same
 * thing in a struct the launcher was handed and could write; the case is the
 * same, and what it now protects is a flag no launcher can reach.
 */
static int case_double_release_forbidden(void) {
    fake_launcher_t fake;
    fake_launcher_init(&fake, FAKE_SERVE_NATIVE);
    maelys_mcp_process_launcher_t *launcher = fake_launcher_create(&fake);
    ASSERT_TRUE(launcher != NULL);
    char *const argv[] = {(char *)"/fake/provider", NULL};
    maelys_mcp_process_launch_params_t params = launch_params("/fake/provider",
        argv, MAELYS_MCP_PROCESS_FD_STDIO);
    maelys_mcp_process_slot_t slot = empty_slot();
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_process_launch(launcher, &params, &slot, &error) ==
        MAELYS_MCP_OK);
    close(slot.protocol_fd);
    ASSERT_TRUE(maelys_mcp_process_shutdown(&slot, 500u, 500u, &error) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_process_shutdown(&slot, 500u, 500u, &error) ==
        MAELYS_MCP_OK);
    /* And the abandon form too: the proxy's failure path calls it and then
     * lets proxy_destroy run the full ladder over the same slot. */
    maelys_mcp_process_abandon(&slot, 500u);
    ASSERT_TRUE(fake.release_count == 1);
    free(error);
    maelys_mcp_process_launcher_release(launcher);
    fake_launcher_clear(&fake);
    return 0;
}

/*
 * 9. A NULL handle is not a released handle. This is the case that fails the
 * day someone implements exactly-once against handle nullity instead of
 * against the runtime's own flag - a stateless launcher needs no per-instance
 * state, and skipping its teardown would be silent.
 */
static int case_null_handle_lifecycle(void) {
    int before = open_descriptor_count();
    fake_launcher_t fake;
    fake_launcher_init(&fake, FAKE_SERVE_NATIVE);
    fake.handle_style = HANDLE_NULL;
    maelys_mcp_process_launcher_t *launcher = fake_launcher_create(&fake);
    ASSERT_TRUE(launcher != NULL);
    maelys_mcp_provider_process_options_t options = native_options(5000u, 1000u);
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options, launcher,
        &provider, &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    ASSERT_TRUE(call_directly(provider, "probe", &error) == MAELYS_MCP_OK);
    free(error);
    maelys_mcp_provider_destroy(provider);
    ASSERT_TRUE(fake.handle_seen_in_stop == 1);
    ASSERT_TRUE(fake.handle_seen_in_wait == 1);
    ASSERT_TRUE(fake.handle_seen_in_release == 1);
    ASSERT_TRUE(fake.last_handle == NULL);
    ASSERT_TRUE(fake.release_count == 1);
    maelys_mcp_process_launcher_release(launcher);
    fake_launcher_clear(&fake);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/*
 * 10. Invalid fd from the launcher. spawn says OK and hands over something the
 * runtime cannot use; the runtime stops the child, releases it, gives back the
 * reference it had just taken, and fails loudly with the launcher named -
 * because "the runtime survives a broken launcher" is the property a pluggable
 * seam has to earn.
 */
static int case_invalid_fd(void) {
    int before = open_descriptor_count();
    fake_launcher_t fake;
    fake_launcher_init(&fake, FAKE_INVALID_FD);
    maelys_mcp_process_launcher_t *launcher = fake_launcher_create(&fake);
    ASSERT_TRUE(launcher != NULL);
    maelys_mcp_provider_process_options_t options = native_options(1000u, 200u);
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options, launcher,
        &provider, &error) == MAELYS_MCP_ERR_PROTOCOL);
    ASSERT_TRUE(provider == NULL);
    ASSERT_TRUE(error != NULL && strstr(error, "fake") != NULL);
    ASSERT_TRUE(fake.forced_stops == 1);
    ASSERT_TRUE(fake.graceful_stops == 0);
    ASSERT_TRUE(fake.release_count == 1);
    free(error);
    maelys_mcp_process_launcher_release(launcher);
    fake_launcher_clear(&fake);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/*
 * 11. Both provider kinds through ONE launcher, which is the argument the
 * whole seam rests on: a policy enforcement point that governs native
 * providers but not proxied upstreams governs nothing. It also pins the
 * layout derivation - the proxy's request must say STDIO because an MCP server
 * speaks stdin and stdout by specification, not because nobody set the field -
 * and the argv asymmetry, which is the same fact seen from the other side.
 */
static int case_both_kinds_one_launcher(void) {
    int before = open_descriptor_count();
    fake_launcher_t native_fake;
    fake_launcher_init(&native_fake, FAKE_SERVE_NATIVE);
    maelys_mcp_process_launcher_t *native_launcher =
        fake_launcher_create(&native_fake);
    ASSERT_TRUE(native_launcher != NULL);
    fake_launcher_t proxy_fake;
    fake_launcher_init(&proxy_fake, FAKE_SERVE_MCP);
    maelys_mcp_process_launcher_t *proxy_launcher =
        fake_launcher_create(&proxy_fake);
    ASSERT_TRUE(proxy_launcher != NULL);

    maelys_mcp_provider_process_options_t options = native_options(5000u, 500u);
    maelys_mcp_provider_t *native = NULL;
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options,
        native_launcher, &native, &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    maelys_mcp_proxy_options_t upstream = proxy_options(5000u);
    maelys_mcp_provider_t *proxy = NULL;
    ASSERT_TRUE(maelys_mcp_provider_proxy_spawn_with_launcher(&upstream,
        proxy_launcher, &proxy, NULL, &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    ASSERT_TRUE(call_directly(native, "probe", &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    ASSERT_TRUE(call_directly(proxy, "probe", &error) == MAELYS_MCP_OK);
    free(error);
    ASSERT_TRUE(native_fake.last_layout == MAELYS_MCP_PROCESS_FD_STDIO);
    ASSERT_TRUE(proxy_fake.last_layout == MAELYS_MCP_PROCESS_FD_STDIO);
    /*
     * The complete vector reached the launcher, argv[0] included. The native
     * kind's is compiled by the runtime - --provider is what puts a
     * maelys-provider binary into provider mode - and the proxy kind's is the
     * caller's whole vector, because a third-party MCP server has an arbitrary
     * CLI the runtime holds no invariant about.
     */
    ASSERT_TRUE(strcmp(native_fake.last_arguments,
        "/fake/provider|--provider") == 0);
    ASSERT_TRUE(native_fake.last_arg_count == 2u);
    ASSERT_TRUE(strcmp(proxy_fake.last_arguments, "/fake/upstream") == 0);
    ASSERT_TRUE(proxy_fake.last_arg_count == 1u);
    maelys_mcp_provider_destroy(native);
    maelys_mcp_provider_destroy(proxy);
    ASSERT_TRUE(native_fake.spawn_count == 1 && native_fake.release_count == 1);
    ASSERT_TRUE(proxy_fake.spawn_count == 1 && proxy_fake.release_count == 1);
    maelys_mcp_process_launcher_release(native_launcher);
    maelys_mcp_process_launcher_release(proxy_launcher);
    fake_launcher_clear(&native_fake);
    fake_launcher_clear(&proxy_fake);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/*
 * 12. The APIs that predate the seam still behave as they did. The existing
 * suites are the real evidence for this - none of them was touched - and this
 * case adds the one assertion they cannot make: that the old entry point and
 * the new one reach the same provider through the same kind of launcher.
 */
static int case_old_apis_unchanged(const char *example_provider) {
    int before = open_descriptor_count();
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    maelys_mcp_provider_process_options_t options = native_options(30000u, 2000u);
    options.executable_path = example_provider;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_options(&options, &provider,
        &error) == MAELYS_MCP_OK);
    free(error);
    maelys_mcp_provider_destroy(provider);

    provider = NULL;
    error = NULL;
    maelys_mcp_process_launcher_t *posix_launcher = NULL;
    ASSERT_TRUE(maelys_mcp_posix_launcher_create(&posix_launcher, &error) ==
        MAELYS_MCP_OK);
    free(error);
    error = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options,
        posix_launcher, &provider, &error) == MAELYS_MCP_OK);
    free(error);
    /*
     * The caller's reference goes back HERE, while the provider is still very
     * much alive and about to be used. Under ABI 4 this was a use-after-free
     * waiting at the next teardown; the whole point of the refcount is that it
     * is now merely correct, and needs no ordering against anything.
     */
    maelys_mcp_process_launcher_release(posix_launcher);
    error = NULL;
    /* Still talking, over a transport whose launcher the caller has already
     * let go of: a real call to the real child, answered. */
    ASSERT_TRUE(call_echo(provider, &error) == MAELYS_MCP_OK);
    free(error);
    maelys_mcp_provider_destroy(provider);
    ASSERT_TRUE(open_descriptor_count() == before);

    /* Naming no launcher at all is an argument error, not a silent default. */
    provider = NULL;
    error = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options, NULL, &provider,
        &error) == MAELYS_MCP_ERR_ARGUMENT);
    free(error);
    error = NULL;
    maelys_mcp_proxy_options_t upstream = proxy_options(1000u);
    ASSERT_TRUE(maelys_mcp_provider_proxy_spawn_with_launcher(&upstream, NULL,
        &provider, NULL, &error) == MAELYS_MCP_ERR_ARGUMENT);
    free(error);
    return 0;
}

/*
 * 13. Handle opacity. The launcher returns something that is not a pointer to
 * anything, and the runtime round-trips it through wait, stop and release
 * untouched. This is the test that fails the day someone "optimizes" the
 * handle back into a pid_t.
 */
static int case_handle_opacity(void) {
    int before = open_descriptor_count();
    fake_launcher_t fake;
    fake_launcher_init(&fake, FAKE_SERVE_NATIVE);
    fake.handle_style = HANDLE_OPAQUE;
    maelys_mcp_process_launcher_t *launcher = fake_launcher_create(&fake);
    ASSERT_TRUE(launcher != NULL);
    maelys_mcp_provider_process_options_t options = native_options(5000u, 1000u);
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options, launcher,
        &provider, &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    ASSERT_TRUE(call_directly(provider, "probe", &error) == MAELYS_MCP_OK);
    free(error);
    maelys_mcp_provider_destroy(provider);
    ASSERT_TRUE(fake.last_handle == FAKE_OPAQUE_HANDLE);
    ASSERT_TRUE(fake.handle_seen_in_wait && fake.handle_seen_in_stop &&
        fake.handle_seen_in_release);
    ASSERT_TRUE(fake.release_count == 1);
    maelys_mcp_process_launcher_release(launcher);
    fake_launcher_clear(&fake);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/*
 * 14. The POSIX launcher's own refusals, both of them.
 *
 * An executionProfile it does not implement is a request for confinement it
 * cannot provide, and starting the child anyway would answer a security
 * request with an unsandboxed process and no diagnostic. An environment
 * platform it does not implement is the same mistake about a different fact:
 * it forks on this machine, so any other token names a target it cannot reach.
 *
 * The platform half has to build a request by hand, because the runtime
 * produces only "local" and there is deliberately no way to ask it for
 * anything else this release. That is whitebox - the suite ships inside the
 * library that owns the struct - and it is the only way to exercise a refusal
 * whose trigger is host configuration that does not exist yet.
 */
static int case_posix_refusals(const char *example_provider) {
    char *error = NULL;
    maelys_mcp_process_launcher_t *posix_launcher = NULL;
    ASSERT_TRUE(maelys_mcp_posix_launcher_create(&posix_launcher, &error) ==
        MAELYS_MCP_OK);
    free(error);
    error = NULL;
    char *const argv[] = {(char *)example_provider, (char *)"--provider", NULL};
    maelys_mcp_process_launch_params_t params = launch_params(example_provider,
        argv, MAELYS_MCP_PROCESS_FD_STDIO);
    params.execution_profile = "seatbelt-readonly";
    maelys_mcp_process_slot_t slot = empty_slot();
    int before = open_descriptor_count();
    ASSERT_TRUE(maelys_mcp_process_launch(posix_launcher, &params, &slot,
        &error) == MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(error != NULL && strstr(error, "seatbelt-readonly") != NULL);
    ASSERT_TRUE(slot.live == 0);
    free(error);
    error = NULL;
    ASSERT_TRUE(open_descriptor_count() == before);

    /* The two profiles it does accept: absent, and the explicit spelling of
     * absent. */
    params.execution_profile = "trusted-local";
    ASSERT_TRUE(maelys_mcp_process_launch(posix_launcher, &params, &slot,
        &error) == MAELYS_MCP_OK);
    close(slot.protocol_fd);
    ASSERT_TRUE(maelys_mcp_process_shutdown(&slot, 500u, 500u, &error) ==
        MAELYS_MCP_OK);
    params.execution_profile = NULL;
    ASSERT_TRUE(maelys_mcp_process_launch(posix_launcher, &params, &slot,
        &error) == MAELYS_MCP_OK);
    close(slot.protocol_fd);
    ASSERT_TRUE(maelys_mcp_process_shutdown(&slot, 500u, 500u, &error) ==
        MAELYS_MCP_OK);
    free(error);
    error = NULL;
    ASSERT_TRUE(open_descriptor_count() == before);

    /*
     * A foreign platform, refused by name. The refusal has to name the token,
     * because a pinned adapter can print a token it does not implement and an
     * operator can act on that - which is the argument the header makes for a
     * string rather than an enum, and this is where it is checked.
     */
    maelys_mcp_process_request_t foreign;
    memset(&foreign, 0, sizeof(foreign));
    foreign.executable = example_provider;
    foreign.argv = argv;
    foreign.arg_count = 2u;
    foreign.fd_layout = MAELYS_MCP_PROCESS_FD_STDIO;
    foreign.force_timeout_ms = 500u;
    foreign.environment_platform = "linux-musl-aarch64";
    foreign.environment_names[0] = "LANG";
    foreign.environment_values[0] = "C";
    foreign.environment_count = 1u;
    void *handle = (void *)(uintptr_t)0xbadbad;
    int protocol_fd = 4242;
    ASSERT_TRUE(posix_launcher->ops->spawn(posix_launcher->context, &foreign,
        &handle, &protocol_fd, &error) == MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(error != NULL && strstr(error, "linux-musl-aarch64") != NULL);
    /* Neither out parameter written on a failed spawn: the caller's own
     * initializers are still there. */
    ASSERT_TRUE(handle == (void *)(uintptr_t)0xbadbad);
    ASSERT_TRUE(protocol_fd == 4242);
    free(error);
    maelys_mcp_process_launcher_release(posix_launcher);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/*
 * 15. MAELYS_MCP_PROCESS_FD_ISOLATED, driven directly through the seam - no
 * manifest key or options field selects it this release
 * (docs/launch-contract-design.md, "The layout is a function of the child's
 * protocol type"). The real C SDK provider completes a real describe -> call
 * -> shutdown session entirely over fd 3, proving the launcher's arrangement,
 * the environment the runtime compiled for it, and the SDK's adoption of
 * MAELYS_PROVIDER_FD actually interoperate - not just each side's half in
 * isolation.
 */
static int case_isolated_layout(const char *example_provider) {
    int before = open_descriptor_count();
    char *error = NULL;
    maelys_mcp_process_launcher_t *posix_launcher = NULL;
    ASSERT_TRUE(maelys_mcp_posix_launcher_create(&posix_launcher, &error) ==
        MAELYS_MCP_OK);
    free(error);
    error = NULL;
    char *const argv_vector[] = {(char *)example_provider, (char *)"--provider", NULL};
    maelys_mcp_process_launch_params_t params = launch_params(example_provider,
        argv_vector, MAELYS_MCP_PROCESS_FD_ISOLATED);
    params.grace_timeout_ms = 2000u;
    params.force_timeout_ms = 2000u;
    maelys_mcp_process_slot_t slot = empty_slot();
    ASSERT_TRUE(maelys_mcp_process_launch(posix_launcher, &params, &slot,
        &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;

    maelys_mcp_line_reader_t reader;
    ASSERT_TRUE(maelys_mcp_line_reader_init(&reader, 65536u) == MAELYS_MCP_OK);

    json_t *describe = json_pack("{s:s,s:i,s:s,s:{}}",
        "protocol", MAELYS_MCP_PROVIDER_PROTOCOL_FLOOR,
        "id", 1, "method", "provider/describe", "params");
    ASSERT_TRUE(describe != NULL);
    ASSERT_TRUE(maelys_mcp_write_json_line(slot.protocol_fd, describe) ==
        MAELYS_MCP_OK);
    json_decref(describe);
    json_t *message = NULL;
    ASSERT_TRUE(maelys_mcp_line_reader_read(&reader, slot.protocol_fd,
        &message, &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    json_t *result = json_object_get(message, "result");
    ASSERT_TRUE(json_is_object(result));
    ASSERT_TRUE(maelys_mcp_json_string_equals(json_object_get(result, "name"),
        "example"));
    json_decref(message);

    json_t *call = json_pack("{s:s,s:i,s:s,s:{s:s,s:{s:s}}}",
        "protocol", MAELYS_MCP_PROVIDER_PROTOCOL_FLOOR,
        "id", 2, "method", "provider/call",
        "params", "name", "example.echo", "arguments", "message", "over fd 3");
    ASSERT_TRUE(call != NULL);
    ASSERT_TRUE(maelys_mcp_write_json_line(slot.protocol_fd, call) ==
        MAELYS_MCP_OK);
    json_decref(call);
    message = NULL;
    ASSERT_TRUE(maelys_mcp_line_reader_read(&reader, slot.protocol_fd,
        &message, &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    result = json_object_get(message, "result");
    json_t *structured = json_is_object(result) ?
        json_object_get(result, "structuredContent") : NULL;
    ASSERT_TRUE(maelys_mcp_json_string_equals(
        json_object_get(structured, "message"), "over fd 3"));
    json_decref(message);

    json_t *goodbye = json_pack("{s:s,s:i,s:s,s:{}}",
        "protocol", MAELYS_MCP_PROVIDER_PROTOCOL_FLOOR,
        "id", 3, "method", "provider/shutdown", "params");
    ASSERT_TRUE(goodbye != NULL);
    ASSERT_TRUE(maelys_mcp_write_json_line(slot.protocol_fd, goodbye) ==
        MAELYS_MCP_OK);
    json_decref(goodbye);
    message = NULL;
    ASSERT_TRUE(maelys_mcp_line_reader_read(&reader, slot.protocol_fd,
        &message, &error) == MAELYS_MCP_OK);
    free(error);
    json_decref(message);
    maelys_mcp_line_reader_clear(&reader);

    close(slot.protocol_fd);
    error = NULL;
    ASSERT_TRUE(maelys_mcp_process_shutdown(&slot, 2000u, 2000u, &error) ==
        MAELYS_MCP_OK);
    free(error);
    maelys_mcp_process_launcher_release(posix_launcher);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/*
 * 16. Old-SDK tolerability under ISOLATED. The stubborn fixture
 * (tests/helpers/adversarial_provider.c) reads stdin and writes stdout
 * directly and has no notion of MAELYS_PROVIDER_FD at all - exactly what a
 * first-party SDK shipped before this change looks like. Spawned under
 * ISOLATED, its fd 0 is /dev/null, so its first fgets() returns EOF
 * immediately and it exits clean before ever answering describe
 * (docs/launch-contract-design.md, "Correcting the framing: this is a
 * declaration, not a negotiation"). What this proves is not merely that it
 * dies, but that the runtime notices FAST, through the fd, rather than
 * hanging to the describe deadline - so the assertion is a loose wall-clock
 * bound against a fixture that should answer in milliseconds, not the
 * generous 30s budget the other real-child cases in this file give a fixture
 * that is expected to actually take its time.
 */
static int case_isolated_old_sdk_tolerability(const char *stubborn_executable) {
    int before = open_descriptor_count();
    char *error = NULL;
    maelys_mcp_process_launcher_t *posix_launcher = NULL;
    ASSERT_TRUE(maelys_mcp_posix_launcher_create(&posix_launcher, &error) ==
        MAELYS_MCP_OK);
    free(error);
    error = NULL;
    char *const argv_vector[] = {
        (char *)stubborn_executable, (char *)"--provider", NULL
    };
    maelys_mcp_process_launch_params_t params = launch_params(
        stubborn_executable, argv_vector, MAELYS_MCP_PROCESS_FD_ISOLATED);
    maelys_mcp_process_slot_t slot = empty_slot();
    ASSERT_TRUE(maelys_mcp_process_launch(posix_launcher, &params, &slot,
        &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;

    maelys_mcp_line_reader_t reader;
    ASSERT_TRUE(maelys_mcp_line_reader_init(&reader, 65536u) == MAELYS_MCP_OK);
    long long started = milliseconds();
    json_t *message = NULL;
    /* EOF, not a describe response: the fixture never reads
     * MAELYS_PROVIDER_FD and speaks plain stdin/stdout, which under ISOLATED
     * are /dev/null and stderr. Its own fgets() sees immediate EOF and it
     * exits without ever writing a byte to fd 3. */
    maelys_mcp_result_t status = maelys_mcp_line_reader_read(&reader,
        slot.protocol_fd, &message, &error);
    long long elapsed = milliseconds() - started;
    ASSERT_TRUE(status == MAELYS_MCP_ERR_NOT_FOUND);
    ASSERT_TRUE(message == NULL);
    free(error);
    error = NULL;
    /* Fast, not just eventually: an old SDK under ISOLATED must read as "died
     * before describe", never as "hung until the describe deadline". */
    ASSERT_TRUE(elapsed < 3000LL);
    maelys_mcp_line_reader_clear(&reader);

    close(slot.protocol_fd);
    ASSERT_TRUE(maelys_mcp_process_shutdown(&slot, 500u, 500u, &error) ==
        MAELYS_MCP_OK);
    free(error);
    maelys_mcp_process_launcher_release(posix_launcher);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/* ------------------------------------------------------------ ABI 5 cases */

/*
 * 17. The check that is the point of the vtable. An ops table is the one
 * structure in the header that crosses a build boundary, so a mismatch there
 * is a call through a function pointer at the wrong offset with the wrong
 * signature - which no later check catches and no sanitizer report explains.
 *
 * Both numbers have to be in the message. "ABI mismatch" tells an operator
 * that something is wrong; "built against 4, this library implements 5" tells
 * them which of the two binaries to rebuild.
 */
static int case_abi_version_mismatch(void) {
    fake_launcher_t fake;
    fake_launcher_init(&fake, FAKE_SERVE_NATIVE);
    /* One below and one above: not ">=" and not "near enough", in either
     * direction. Zero is the third, because a zero-initialized ops table is
     * what a launcher that forgot the field entirely produces. */
    const unsigned int wrong_versions[] = {
        MAELYS_MCP_ABI_VERSION - 1u, MAELYS_MCP_ABI_VERSION + 1u, 0u
    };
    for (size_t index = 0; index < 3u; ++index) {
        maelys_mcp_process_ops_t mismatched = fake_ops;
        mismatched.abi_version = wrong_versions[index];
        maelys_mcp_process_launcher_t *launcher = NULL;
        char *error = NULL;
        ASSERT_TRUE(maelys_mcp_process_launcher_create("mismatched",
            &mismatched, &fake, NULL, &launcher, &error) ==
            MAELYS_MCP_ERR_ARGUMENT);
        /* No launcher created, and *out_launcher untouched. */
        ASSERT_TRUE(launcher == NULL);
        ASSERT_TRUE(error != NULL);
        char theirs[32];
        char ours[32];
        (void)snprintf(theirs, sizeof(theirs), "%u", wrong_versions[index]);
        (void)snprintf(ours, sizeof(ours), "%u",
            (unsigned int)MAELYS_MCP_ABI_VERSION);
        ASSERT_TRUE(strstr(error, theirs) != NULL);
        ASSERT_TRUE(strstr(error, ours) != NULL);
        ASSERT_TRUE(strstr(error, "mismatched") != NULL);
        free(error);
    }
    /* No op was ever called: no context was taken over and nothing was
     * started. */
    ASSERT_TRUE(fake.spawn_count == 0);
    ASSERT_TRUE(fake.release_count == 0);

    /* The rest of create's refusals, which the ABI check must not have
     * displaced. */
    maelys_mcp_process_launcher_t *launcher = NULL;
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_process_launcher_create(NULL, &fake_ops, &fake, NULL,
        &launcher, &error) == MAELYS_MCP_ERR_ARGUMENT);
    free(error);
    error = NULL;
    ASSERT_TRUE(maelys_mcp_process_launcher_create("", &fake_ops, &fake, NULL,
        &launcher, &error) == MAELYS_MCP_ERR_ARGUMENT);
    free(error);
    error = NULL;
    ASSERT_TRUE(maelys_mcp_process_launcher_create("fake", NULL, &fake, NULL,
        &launcher, &error) == MAELYS_MCP_ERR_ARGUMENT);
    free(error);
    error = NULL;
    maelys_mcp_process_ops_t incomplete = fake_ops;
    incomplete.wait = NULL;
    ASSERT_TRUE(maelys_mcp_process_launcher_create("fake", &incomplete, &fake,
        NULL, &launcher, &error) == MAELYS_MCP_ERR_ARGUMENT);
    free(error);
    ASSERT_TRUE(launcher == NULL);

    /* And the name that a correct create keeps, which the containment
     * diagnostic is built from. */
    maelys_mcp_process_launcher_t *named = fake_launcher_create(&fake);
    ASSERT_TRUE(named != NULL);
    ASSERT_TRUE(strcmp(maelys_mcp_process_launcher_name(named), "fake") == 0);
    ASSERT_TRUE(maelys_mcp_process_launcher_name(NULL) == NULL);
    maelys_mcp_process_launcher_release(named);
    fake_launcher_clear(&fake);
    return 0;
}

/* The counted context, for the standalone refcount case. */
typedef struct counted_context {
    int releases;
} counted_context_t;

static void counted_release(void *context) {
    counted_context_t *counted = context;
    ++counted->releases;
}

/*
 * 18. release_context runs exactly once, at zero, and not before. NULL is a
 * no-op at both ends, which is what lets a caller write release without
 * checking.
 */
static int case_release_context_exactly_once(void) {
    counted_context_t counted = {.releases = 0};
    maelys_mcp_process_launcher_t *launcher = NULL;
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_process_launcher_create("counted", &fake_ops,
        &counted, counted_release, &launcher, &error) == MAELYS_MCP_OK);
    free(error);
    maelys_mcp_process_launcher_retain(launcher);
    maelys_mcp_process_launcher_retain(launcher);
    ASSERT_TRUE(counted.releases == 0);
    maelys_mcp_process_launcher_release(launcher);
    ASSERT_TRUE(counted.releases == 0);
    maelys_mcp_process_launcher_release(launcher);
    ASSERT_TRUE(counted.releases == 0);
    maelys_mcp_process_launcher_release(launcher);
    ASSERT_TRUE(counted.releases == 1);

    /* NULL is a no-op at both ends. */
    maelys_mcp_process_launcher_retain(NULL);
    maelys_mcp_process_launcher_release(NULL);

    /*
     * A failed create does NOT call release_context and does not take over the
     * context: the caller still owns what it passed, and a create that ran the
     * releaser on the refusal path would free it twice.
     */
    counted_context_t untouched = {.releases = 0};
    maelys_mcp_process_ops_t mismatched = fake_ops;
    mismatched.abi_version = MAELYS_MCP_ABI_VERSION + 1u;
    maelys_mcp_process_launcher_t *refused = NULL;
    error = NULL;
    ASSERT_TRUE(maelys_mcp_process_launcher_create("counted", &mismatched,
        &untouched, counted_release, &refused, &error) ==
        MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(untouched.releases == 0);
    ASSERT_TRUE(refused == NULL);
    free(error);
    return 0;
}

typedef struct teardown_job {
    maelys_mcp_provider_t *provider;
} teardown_job_t;

static void *teardown_main(void *opaque) {
    teardown_job_t *job = opaque;
    maelys_mcp_provider_destroy(job->provider);
    return NULL;
}

/*
 * 19. The lifetime rule ABI 4 stated and could not keep: the RUNTIME RETAINS
 * PER PROVIDER, so the caller's reference is independent of every provider's
 * and releasing it immediately after spawning is correct.
 *
 * Two providers are spawned from one launcher, the caller drops its reference
 * while both are alive, and the two are then torn down on two different
 * threads. Whichever finishes last drops the last reference and runs
 * release_context on its own thread, which is exactly the situation the header
 * says the refcount must be thread-safe for - and why this case is in the TSan
 * run rather than only the plain one.
 *
 * Under ABI 4 this was a use-after-free at the first teardown. The assertion
 * that it is not is the release counter: 0 while any provider is alive, 1
 * afterwards, and never 2.
 */
static int case_launcher_refcount_lifetime(void) {
    int before = open_descriptor_count();
    fake_launcher_t fake;
    fake_launcher_init(&fake, FAKE_SERVE_NATIVE);
    /* One launcher, whose context is the fake and whose release_context counts
     * through that same fake: the ops and the releaser see one object, so "the
     * last reference ran the releaser once" is checkable without a second
     * launcher standing in for the first. */
    maelys_mcp_process_launcher_t *launcher = NULL;
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_process_launcher_create("fake", &fake_ops, &fake,
        fake_context_release, &launcher, &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;

    maelys_mcp_provider_process_options_t options = native_options(5000u, 1000u);
    maelys_mcp_provider_t *first = NULL;
    maelys_mcp_provider_t *second = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options, launcher,
        &first, &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options, launcher,
        &second, &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;

    /*
     * The caller's reference goes back while both providers are alive and
     * about to be used. Under ABI 4 this was the use-after-free: the launcher
     * was a borrowed pointer, and the first teardown would have read a freed
     * ops table. Here it is merely correct, and the releaser NOT having run is
     * the assertion that says so.
     */
    maelys_mcp_process_launcher_release(launcher);
    ASSERT_TRUE(fake.context_releases == 0);
    ASSERT_TRUE(call_directly(first, "probe", &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    ASSERT_TRUE(call_directly(second, "probe", &error) == MAELYS_MCP_OK);
    free(error);
    ASSERT_TRUE(fake.context_releases == 0);

    /* Two teardowns, two threads. Neither is "the owner"; the last one to
     * finish drops the last reference and runs the releaser on its own
     * thread, which is the case TSan is here to watch. */
    teardown_job_t jobs[2] = {{.provider = first}, {.provider = second}};
    pthread_t threads[2];
    ASSERT_TRUE(pthread_create(&threads[0], NULL, teardown_main, &jobs[0]) == 0);
    ASSERT_TRUE(pthread_create(&threads[1], NULL, teardown_main, &jobs[1]) == 0);
    ASSERT_TRUE(pthread_join(threads[0], NULL) == 0);
    ASSERT_TRUE(pthread_join(threads[1], NULL) == 0);
    /* Two children released, one launcher context released - once, not twice
     * and not never. */
    ASSERT_TRUE(fake.release_count == 2);
    ASSERT_TRUE(fake.context_releases == 1);
    fake_launcher_clear(&fake);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/*
 * 20. The request, as the launcher sees it. Everything asserted here crossed
 * the seam through the public getters, which is the difference between testing
 * the contract and testing the runtime's intentions.
 *
 * The environment is the part ABI 5 moved: a closed allowlist compiled by the
 * runtime, in the runtime's order, with no duplicates, and NOT a filtered
 * inheritance of this process's environment - so the assertion is on the exact
 * list, not on "PATH is present". Under ISOLATED it gains MAELYS_PROVIDER_FD
 * and nothing else, which is the one variable whose value is a fact about the
 * descriptor arrangement.
 */
static int case_request_contents(void) {
    int before = open_descriptor_count();
    fake_launcher_t fake;
    fake_launcher_init(&fake, FAKE_SERVE_NATIVE);
    maelys_mcp_process_launcher_t *launcher = fake_launcher_create(&fake);
    ASSERT_TRUE(launcher != NULL);

#ifdef __APPLE__
    const char *expected_stdio_environment =
        "PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin|LANG=C|LC_ALL=C";
#else
    const char *expected_stdio_environment =
        "PATH=/usr/local/bin:/usr/bin:/bin|LANG=C|LC_ALL=C";
#endif

    /* Through a provider, which is how every production launch is built. */
    maelys_mcp_provider_process_options_t options = native_options(4321u, 765u);
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options, launcher,
        &provider, &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    ASSERT_TRUE(strcmp(fake.last_executable, "/fake/provider") == 0);
    ASSERT_TRUE(strcmp(fake.last_arguments, "/fake/provider|--provider") == 0);
    ASSERT_TRUE(fake.last_arg_count == 2u);
    /* There is no terminating NULL element to borrow, and the index past the
     * end reads as absent rather than as garbage. */
    ASSERT_TRUE(fake.last_arg_past_end_was_null == 1);
    /* The only token this runtime produces, and the one the LOCAL rule keys
     * off. */
    ASSERT_TRUE(strcmp(fake.last_platform,
        MAELYS_MCP_PROCESS_ENVIRONMENT_PLATFORM_LOCAL) == 0);
    ASSERT_TRUE(strcmp(fake.last_platform, "local") == 0);
    ASSERT_TRUE(fake.last_environment_count == 3u);
    ASSERT_TRUE(strcmp(fake.last_environment, expected_stdio_environment) == 0);
    ASSERT_TRUE(fake.last_environment_past_end_wrote_nothing == 1);
    /* An absent execution profile is absent, not the empty string: the
     * launcher's own default and an explicit empty request are different
     * things. */
    ASSERT_TRUE(fake.last_profile_absent == 1);
    /* The budgets, carried from the options the caller set. */
    ASSERT_TRUE(fake.last_spawn_timeout_ms == 4321u);
    ASSERT_TRUE(fake.last_grace_timeout_ms == 765u);
    ASSERT_TRUE(fake.last_force_timeout_ms == 765u);
    maelys_mcp_provider_destroy(provider);
    maelys_mcp_process_launcher_release(launcher);
    fake_launcher_clear(&fake);

    /* And under ISOLATED, where the runtime adds exactly one variable. */
    fake_launcher_t isolated_fake;
    fake_launcher_init(&isolated_fake, FAKE_SILENT);
    maelys_mcp_process_launcher_t *isolated_launcher =
        fake_launcher_create(&isolated_fake);
    ASSERT_TRUE(isolated_launcher != NULL);
    char *const argv[] = {(char *)"/fake/provider", (char *)"--provider", NULL};
    maelys_mcp_process_launch_params_t params = launch_params("/fake/provider",
        argv, MAELYS_MCP_PROCESS_FD_ISOLATED);
    params.execution_profile = "trusted-local";
    maelys_mcp_process_slot_t slot = empty_slot();
    error = NULL;
    ASSERT_TRUE(maelys_mcp_process_launch(isolated_launcher, &params, &slot,
        &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(isolated_fake.last_layout == MAELYS_MCP_PROCESS_FD_ISOLATED);
    ASSERT_TRUE(isolated_fake.last_environment_count == 4u);
    char expected_isolated[512];
    (void)snprintf(expected_isolated, sizeof(expected_isolated),
        "%s|MAELYS_PROVIDER_FD=3", expected_stdio_environment);
    ASSERT_TRUE(strcmp(isolated_fake.last_environment, expected_isolated) == 0);
    /* The descriptor the launcher is told to use and the number the child is
     * told to look at are one definition, not two that happen to agree. */
    ASSERT_TRUE(MAELYS_MCP_PROCESS_ISOLATED_FD == 3);
    ASSERT_TRUE(isolated_fake.last_profile_absent == 0);
    ASSERT_TRUE(strcmp(isolated_fake.last_profile, "trusted-local") == 0);
    close(slot.protocol_fd);
    ASSERT_TRUE(maelys_mcp_process_shutdown(&slot, 200u, 200u, &error) ==
        MAELYS_MCP_OK);
    free(error);
    maelys_mcp_process_launcher_release(isolated_launcher);
    fake_launcher_clear(&isolated_fake);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/*
 * 21. Exit status, through the launcher that actually has one. ABI 4 asked
 * wait for a single int, so "the provider died" and "the provider was killed
 * by SIGKILL after ignoring SIGTERM" reached an operator as the same sentence.
 *
 * Two real children, and the two answers must be each other's opposite:
 *
 *   - a fixture that returns 7 reports exited=1, exit_code=7, term_signal=0
 *   - a fixture that ignores SIGTERM and is killed reports exited=1,
 *     exit_code=0, term_signal=SIGKILL
 *
 * Exactly one of the two fields carries the outcome in each, which is the
 * contract's clause and the thing a boolean could not express. The status is
 * also asserted to survive a second wait: a child is reapable once, and a
 * launcher that forgot the answer would be a worse launcher than one that
 * never knew it.
 */
static int case_exit_status_kinds(
    const char *exit_seven_executable,
    const char *stubborn_executable) {
    int before = open_descriptor_count();
    char *error = NULL;
    maelys_mcp_process_launcher_t *posix_launcher = NULL;
    ASSERT_TRUE(maelys_mcp_posix_launcher_create(&posix_launcher, &error) ==
        MAELYS_MCP_OK);
    free(error);
    error = NULL;

    /* Returned 7. */
    char *const seven_argv[] = {(char *)exit_seven_executable, NULL};
    maelys_mcp_process_launch_params_t params = launch_params(
        exit_seven_executable, seven_argv, MAELYS_MCP_PROCESS_FD_STDIO);
    maelys_mcp_process_slot_t slot = empty_slot();
    ASSERT_TRUE(maelys_mcp_process_launch(posix_launcher, &params, &slot,
        &error) == MAELYS_MCP_OK);
    close(slot.protocol_fd);
    maelys_mcp_process_exit_status_t status = {
        .exited = 0, .exit_code = 0, .term_signal = 0
    };
    ASSERT_TRUE(posix_launcher->ops->wait(posix_launcher->context, slot.handle,
        5000u, &status, &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(status.exited == 1);
    ASSERT_TRUE(status.exit_code == 7);
    ASSERT_TRUE(status.term_signal == 0);
    /* Remembered, not re-derived: the second wait gets the same full answer. */
    maelys_mcp_process_exit_status_t again = {
        .exited = 0, .exit_code = 0, .term_signal = 0
    };
    ASSERT_TRUE(posix_launcher->ops->wait(posix_launcher->context, slot.handle,
        5000u, &again, &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(again.exited == 1 && again.exit_code == 7 &&
        again.term_signal == 0);
    ASSERT_TRUE(maelys_mcp_process_shutdown(&slot, 200u, 200u, &error) ==
        MAELYS_MCP_OK);
    free(error);
    error = NULL;

    /* Killed by SIGKILL: the stubborn fixture ignores SIGTERM once it has been
     * asked to shut down, so a graceful stop cannot be what ended it. */
    char *const stubborn_argv[] = {
        (char *)stubborn_executable, (char *)"--provider", NULL
    };
    params = launch_params(stubborn_executable, stubborn_argv,
        MAELYS_MCP_PROCESS_FD_STDIO);
    slot = empty_slot();
    ASSERT_TRUE(maelys_mcp_process_launch(posix_launcher, &params, &slot,
        &error) == MAELYS_MCP_OK);
    json_t *goodbye = json_pack("{s:s,s:i,s:s,s:{}}",
        "protocol", MAELYS_MCP_PROVIDER_PROTOCOL_FLOOR,
        "id", 1, "method", "provider/shutdown", "params");
    ASSERT_TRUE(goodbye != NULL);
    ASSERT_TRUE(maelys_mcp_write_json_line(slot.protocol_fd, goodbye) ==
        MAELYS_MCP_OK);
    json_decref(goodbye);
    struct timespec settle = {.tv_sec = 0, .tv_nsec = 200000000L};
    while (nanosleep(&settle, &settle) != 0) {}
    /* The polite rung first, and it must NOT end this child: if SIGTERM ended
     * it the case would be reporting a signal, but the wrong one, and the
     * assertion below would pass for the wrong reason. */
    ASSERT_TRUE(posix_launcher->ops->stop(posix_launcher->context, slot.handle,
        MAELYS_MCP_PROCESS_STOP_GRACEFUL, &error) == MAELYS_MCP_OK);
    maelys_mcp_process_exit_status_t after_term = {
        .exited = 0, .exit_code = 0, .term_signal = 0
    };
    ASSERT_TRUE(posix_launcher->ops->wait(posix_launcher->context, slot.handle,
        300u, &after_term, &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(after_term.exited == 0);
    ASSERT_TRUE(posix_launcher->ops->stop(posix_launcher->context, slot.handle,
        MAELYS_MCP_PROCESS_STOP_FORCED, &error) == MAELYS_MCP_OK);
    maelys_mcp_process_exit_status_t killed = {
        .exited = 0, .exit_code = 0, .term_signal = 0
    };
    ASSERT_TRUE(posix_launcher->ops->wait(posix_launcher->context, slot.handle,
        5000u, &killed, &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(killed.exited == 1);
    ASSERT_TRUE(killed.term_signal == SIGKILL);
    ASSERT_TRUE(killed.exit_code == 0);
    /* The two outcomes really are each other's opposite, which is the whole
     * claim: neither field is set in both. */
    ASSERT_TRUE(status.exit_code != 0 && status.term_signal == 0);
    ASSERT_TRUE(killed.term_signal != 0 && killed.exit_code == 0);
    close(slot.protocol_fd);
    ASSERT_TRUE(maelys_mcp_process_shutdown(&slot, 200u, 200u, &error) ==
        MAELYS_MCP_OK);
    free(error);
    maelys_mcp_process_launcher_release(posix_launcher);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/*
 * 22. wait and stop can say why, and the ladder keeps the sharpest thing it
 * heard. ABI 4's wait reported an int and took no out_error, and its stop took
 * none either, so a launcher that knew exactly why it could not answer had
 * nowhere to say it and the ladder invented a message from a result code.
 *
 * The launcher here fails BOTH ops, with different messages. The stop fails
 * first, so the stop's message is the one that must survive - "the kill could
 * not be delivered" is a diagnosis, where the wait's "I cannot tell whether it
 * ended" is the consequence of it. A ladder that kept the last message it saw
 * would report the blander one, which is the exact regression this pins.
 */
static int case_wait_stop_error_propagation(void) {
    int before = open_descriptor_count();
    fake_launcher_t fake;
    fake_launcher_init(&fake, FAKE_DIAGNOSTIC_FAILURES);
    maelys_mcp_process_launcher_t *launcher = fake_launcher_create(&fake);
    ASSERT_TRUE(launcher != NULL);
    char *const argv[] = {(char *)"/fake/provider", NULL};
    maelys_mcp_process_launch_params_t params = launch_params("/fake/provider",
        argv, MAELYS_MCP_PROCESS_FD_STDIO);
    maelys_mcp_process_slot_t slot = empty_slot();
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_process_launch(launcher, &params, &slot, &error) ==
        MAELYS_MCP_OK);
    close(slot.protocol_fd);
    ASSERT_TRUE(maelys_mcp_process_shutdown(&slot, 100u, 100u, &error) ==
        MAELYS_MCP_ERR_STATE);
    ASSERT_TRUE(error != NULL);
    /* The sharpest message, the launcher's name, and NOT the blander one that
     * arrived later. */
    ASSERT_TRUE(strstr(error, FAKE_STOP_FAILURE) != NULL);
    ASSERT_TRUE(strstr(error, "fake") != NULL);
    ASSERT_TRUE(strstr(error, FAKE_WAIT_FAILURE) == NULL);
    /* The ladder kept climbing past the failed stop: a stop that could not be
     * delivered is exactly when the next rung matters most. */
    ASSERT_TRUE(fake.graceful_stops == 1);
    ASSERT_TRUE(fake.forced_stops == 1);
    ASSERT_TRUE(fake.wait_calls == 2);
    ASSERT_TRUE(fake.release_count == 1);
    free(error);

    /*
     * A caller that wants no message is entitled to pass NULL at every op, and
     * a launcher must never dereference an out_error it was not given. The
     * abandon path passes NULL all the way down, so driving it over the same
     * failing launcher is the check.
     */
    maelys_mcp_process_slot_t second = empty_slot();
    ASSERT_TRUE(maelys_mcp_process_launch(launcher, &params, &second, NULL) ==
        MAELYS_MCP_OK);
    close(second.protocol_fd);
    maelys_mcp_process_abandon(&second, 100u);
    ASSERT_TRUE(fake.release_count == 2);
    maelys_mcp_process_launcher_release(launcher);
    fake_launcher_clear(&fake);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

int main(int argc, char **argv) {
    /* example-provider, one that answers describe a second late, one that
     * ignores SIGTERM, and one that returns 7 and nothing else. The fifth real
     * child - a binary that exits at once - is /usr/bin/false, named here as
     * tests/test_process_provider.c already names it rather than being built
     * as a fixture. */
    ASSERT_TRUE(argc == 5);
    const char *example_provider = argv[1];
    const char *silent_executable = argv[2];
    const char *stubborn_executable = argv[3];
    const char *exit_seven_executable = argv[4];
    const char *dead_executable = "/usr/bin/false";

    ASSERT_TRUE(case_spawn_failure() == 0);
    ASSERT_TRUE(case_death_before_describe(dead_executable) == 0);
    ASSERT_TRUE(case_describe_timeout(silent_executable) == 0);
    ASSERT_TRUE(case_crash_mid_call() == 0);
    ASSERT_TRUE(case_graceful_stop() == 0);
    ASSERT_TRUE(case_forced_escalation(stubborn_executable) == 0);
    ASSERT_TRUE(case_containment_failure() == 0);
    ASSERT_TRUE(case_double_release_forbidden() == 0);
    ASSERT_TRUE(case_null_handle_lifecycle() == 0);
    ASSERT_TRUE(case_invalid_fd() == 0);
    ASSERT_TRUE(case_both_kinds_one_launcher() == 0);
    ASSERT_TRUE(case_old_apis_unchanged(example_provider) == 0);
    ASSERT_TRUE(case_handle_opacity() == 0);
    ASSERT_TRUE(case_posix_refusals(example_provider) == 0);
    ASSERT_TRUE(case_isolated_layout(example_provider) == 0);
    ASSERT_TRUE(case_isolated_old_sdk_tolerability(stubborn_executable) == 0);
    ASSERT_TRUE(case_abi_version_mismatch() == 0);
    ASSERT_TRUE(case_release_context_exactly_once() == 0);
    ASSERT_TRUE(case_launcher_refcount_lifetime() == 0);
    ASSERT_TRUE(case_request_contents() == 0);
    ASSERT_TRUE(case_exit_status_kinds(exit_seven_executable,
        stubborn_executable) == 0);
    ASSERT_TRUE(case_wait_stop_error_propagation() == 0);
    printf("test_process_launcher: OK\n");
    return 0;
}
