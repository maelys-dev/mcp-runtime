/*
 * Conformance for the process launch seam (docs/launch-contract-design.md).
 *
 * Two things are being tested here, and the second is the reason the file
 * exists. The first is behaviour: spawn failures, a child that dies before it
 * describes, a crash with a call outstanding, the bounded shutdown ladder and
 * its escalation. The second is the *abstraction*: that the runtime never
 * treats the launcher's handle as a pointer worth checking for NULL, never
 * treats it as a pid, and survives a launcher that violates its side of the
 * contract. Those are the two ways this design gets undone by a
 * plausible-looking simplification, so they get cases of their own.
 *
 * Every case runs against a fake launcher that starts no process at all - its
 * "children" are threads on the other end of a socketpair - and the cases that
 * can be expressed with a real child run against the POSIX launcher too.
 * scripts/audit_boundaries.sh asserts that this file creates no process
 * itself: a fork here would mean the fake path was quietly testing the real
 * one.
 */

#include "maelys/mcp.h"

#include "src/internal/internal.h"

#include <fcntl.h>
#include <pthread.h>
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
    /* spawn returns non-OK. Nothing was started, nothing must be destroyed. */
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
    FAKE_UNKILLABLE
} fake_behaviour_t;

typedef enum fake_handle_style {
    /* The handle is a pointer to the launcher's per-instance state. */
    HANDLE_POINTER = 0,
    /* A stateless launcher: NULL is a legitimate handle, and must never be
     * read as "already destroyed". */
    HANDLE_NULL,
    /* A handle that is not a pointer to anything at all. */
    HANDLE_OPAQUE
} fake_handle_style_t;

#define FAKE_OPAQUE_HANDLE ((void *)(uintptr_t)0x5eed)
#define FAKE_MAX_CHILDREN 4

typedef struct fake_launcher fake_launcher_t;

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
    int destroyed;
} fake_child_t;

struct fake_launcher {
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
    int destroy_count;
    /* What the runtime handed back to the ops, recorded so a case can assert
     * the handle round-tripped untouched. */
    void *last_handle;
    int handle_seen_in_wait;
    int handle_seen_in_stop;
    int handle_seen_in_destroy;
    /* What the runtime put in the spec, recorded so a case can assert the
     * layout was derived rather than defaulted into. */
    maelys_mcp_process_fd_layout_t last_layout;
    char last_argv0[256];

    fake_child_t *children[FAKE_MAX_CHILDREN];
    size_t child_count;
};

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
 * destroy(): destroy is defined as local release only and must not join
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
 * cases 9 and 13.
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

static maelys_mcp_result_t fake_spawn(
    void *context,
    const maelys_mcp_process_spec_t *spec,
    maelys_mcp_process_instance_t *out_instance,
    char **out_error) {
    fake_launcher_t *fake = context;
    pthread_mutex_lock(&fake->mutex);
    ++fake->spawn_count;
    fake->last_layout = spec->fd_layout;
    (void)snprintf(fake->last_argv0, sizeof(fake->last_argv0), "%s",
        spec->argv && spec->argv[0] ? spec->argv[0] : "");
    pthread_mutex_unlock(&fake->mutex);

    if (fake->behaviour == FAKE_SPAWN_FAILS) {
        if (out_error) {
            free(*out_error);
            *out_error = maelys_mcp_strdup(fake->spawn_error);
        }
        return fake->spawn_status;
    }
    if (fake->behaviour == FAKE_INVALID_FD) {
        /* A contract violation the runtime is the only side able to detect:
         * this launcher believes it succeeded. */
        out_instance->protocol_fd = -1;
        out_instance->handle = handle_for(fake, NULL);
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
    out_instance->protocol_fd = sockets[0];
    out_instance->handle = handle_for(fake, child);
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t fake_wait(
    void *context,
    void *handle,
    unsigned int timeout_ms,
    int *out_exited) {
    fake_launcher_t *fake = context;
    pthread_mutex_lock(&fake->mutex);
    ++fake->wait_calls;
    fake->last_handle = handle;
    fake->handle_seen_in_wait = 1;
    pthread_mutex_unlock(&fake->mutex);
    *out_exited = 0;
    fake_child_t *child = resolve_child(fake, handle);
    if (!child) {
        *out_exited = 1;
        return MAELYS_MCP_OK;
    }
    if (fake->behaviour == FAKE_UNKILLABLE) {
        /* The honest shape of a containment failure: the budget is spent and
         * the child is still there. */
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
            *out_exited = 1;
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
    maelys_mcp_process_stop_t mode) {
    fake_launcher_t *fake = context;
    pthread_mutex_lock(&fake->mutex);
    if (mode == MAELYS_MCP_PROCESS_STOP_FORCED) ++fake->forced_stops;
    else ++fake->graceful_stops;
    fake->last_handle = handle;
    fake->handle_seen_in_stop = 1;
    pthread_mutex_unlock(&fake->mutex);
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

static void fake_destroy(void *context, void *handle) {
    fake_launcher_t *fake = context;
    pthread_mutex_lock(&fake->mutex);
    ++fake->destroy_count;
    fake->last_handle = handle;
    fake->handle_seen_in_destroy = 1;
    pthread_mutex_unlock(&fake->mutex);
    fake_child_t *child = resolve_child(fake, handle);
    /* Local release only. The thread and its descriptor are the launcher's own
     * bookkeeping and are reclaimed in fake_launcher_clear, because destroy is
     * not allowed to wait for anything. */
    if (child) child->destroyed = 1;
}

static const maelys_mcp_process_ops_t fake_ops = {
    .spawn = fake_spawn,
    .wait = fake_wait,
    .stop = fake_stop,
    .destroy = fake_destroy
};

static maelys_mcp_process_launcher_t fake_launcher_of(fake_launcher_t *fake) {
    maelys_mcp_process_launcher_t launcher = {
        .name = "fake", .ops = &fake_ops, .context = fake
    };
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

/* ------------------------------------------------------------------ the cases */

/* 1. Spawn failure. No fd, no child, no destroy, and the launcher's own error
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
        maelys_mcp_process_launcher_t launcher = fake_launcher_of(&fake);
        maelys_mcp_provider_t *provider = NULL;
        char *error = NULL;
        maelys_mcp_result_t status;
        if (kind == 0) {
            maelys_mcp_provider_process_options_t options = native_options(1000u, 200u);
            status = maelys_mcp_provider_spawn_with_launcher(&options, &launcher,
                &provider, &error);
        } else {
            maelys_mcp_proxy_options_t options = proxy_options(1000u);
            status = maelys_mcp_provider_proxy_spawn_with_launcher(&options,
                &launcher, &provider, NULL, &error);
        }
        ASSERT_TRUE(status == MAELYS_MCP_ERR_IO);
        ASSERT_TRUE(provider == NULL);
        ASSERT_TRUE(error != NULL);
        ASSERT_TRUE(strcmp(error, "the fake launcher refused to spawn") == 0);
        ASSERT_TRUE(fake.spawn_count == 1);
        ASSERT_TRUE(fake.destroy_count == 0);
        free(error);
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
    maelys_mcp_process_launcher_t launcher = fake_launcher_of(&fake);
    maelys_mcp_provider_process_options_t options = native_options(10000u, 200u);
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    long long started = milliseconds();
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options, &launcher,
        &provider, &error) == MAELYS_MCP_ERR_PROVIDER);
    ASSERT_TRUE(milliseconds() - started < 5000LL);
    ASSERT_TRUE(provider == NULL);
    ASSERT_TRUE(error != NULL);
    /* The failed spawn still tore the instance down exactly once. */
    ASSERT_TRUE(fake.destroy_count == 1);
    free(error);
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
    maelys_mcp_process_launcher_t launcher = fake_launcher_of(&fake);
    maelys_mcp_provider_process_options_t options = native_options(300u, 200u);
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    long long started = milliseconds();
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options, &launcher,
        &provider, &error) != MAELYS_MCP_OK);
    long long elapsed = milliseconds() - started;
    ASSERT_TRUE(elapsed >= 300LL);
    ASSERT_TRUE(elapsed < 5000LL);
    ASSERT_TRUE(provider == NULL);
    ASSERT_TRUE(fake.destroy_count == 1);
    free(error);
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
        maelys_mcp_process_launcher_t launcher = fake_launcher_of(&fake);
        maelys_mcp_provider_t *provider = NULL;
        char *error = NULL;
        if (kind == 0) {
            maelys_mcp_provider_process_options_t options = native_options(5000u, 200u);
            ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options,
                &launcher, &provider, &error) == MAELYS_MCP_OK);
        } else {
            maelys_mcp_proxy_options_t options = proxy_options(5000u);
            ASSERT_TRUE(maelys_mcp_provider_proxy_spawn_with_launcher(&options,
                &launcher, &provider, NULL, &error) == MAELYS_MCP_OK);
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
        ASSERT_TRUE(fake.destroy_count == 1);
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
    maelys_mcp_process_launcher_t launcher = fake_launcher_of(&fake);
    maelys_mcp_provider_process_options_t options = native_options(5000u, 1000u);
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options, &launcher,
        &provider, &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    ASSERT_TRUE(call_directly(provider, "probe", &error) == MAELYS_MCP_OK);
    free(error);
    maelys_mcp_provider_destroy(provider);
    ASSERT_TRUE(fake.graceful_stops == 1);
    ASSERT_TRUE(fake.forced_stops == 0);
    ASSERT_TRUE(fake.destroy_count == 1);
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
    maelys_mcp_process_launcher_t launcher = fake_launcher_of(&fake);
    maelys_mcp_provider_process_options_t options = native_options(5000u, 300u);
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options, &launcher,
        &provider, &error) == MAELYS_MCP_OK);
    free(error);
    long long started = milliseconds();
    maelys_mcp_provider_destroy(provider);
    long long elapsed = milliseconds() - started;
    ASSERT_TRUE(fake.graceful_stops == 1);
    ASSERT_TRUE(fake.forced_stops == 1);
    ASSERT_TRUE(fake.destroy_count == 1);
    /* Bounded: the grace budget was spent, and nothing beyond the force
     * budget was. */
    ASSERT_TRUE(elapsed >= 300LL);
    ASSERT_TRUE(elapsed < 10000LL);
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
     * see. So the same fixture is driven through the ops directly, where the
     * ladder's own verdict is visible: MAELYS_MCP_OK means the child was
     * observed to exit, and only a genuine SIGKILL can produce that against a
     * process ignoring SIGTERM.
     */
    before = open_descriptor_count();
    const maelys_mcp_process_launcher_t *posix_launcher = maelys_mcp_posix_launcher();
    char *const stubborn_argv[] = {
        (char *)stubborn_executable, (char *)"--provider", NULL
    };
    maelys_mcp_process_spec_t spec = {
        .executable_path = stubborn_executable,
        .argv = stubborn_argv,
        .max_message_bytes = 65536u,
        .grace_timeout_ms = 300u,
        .force_timeout_ms = 2000u,
        .fd_layout = MAELYS_MCP_PROCESS_FD_STDIO
    };
    maelys_mcp_process_instance_t instance = {.protocol_fd = -1};
    ASSERT_TRUE(maelys_mcp_process_launch(posix_launcher, &spec, &instance,
        &error) == MAELYS_MCP_OK);
    /* The fixture ignores SIGTERM only once it has been asked to shut down, so
     * the request has to reach it first - and it never answers. */
    json_t *goodbye = json_pack("{s:s,s:i,s:s,s:{}}",
        "protocol", MAELYS_MCP_PROVIDER_PROTOCOL_FLOOR,
        "id", 1,
        "method", "provider/shutdown",
        "params");
    ASSERT_TRUE(goodbye != NULL);
    ASSERT_TRUE(maelys_mcp_write_json_line(instance.protocol_fd, goodbye) ==
        MAELYS_MCP_OK);
    json_decref(goodbye);
    struct timespec settle = {.tv_sec = 0, .tv_nsec = 200000000L};
    while (nanosleep(&settle, &settle) != 0) {}
    close(instance.protocol_fd);
    started = milliseconds();
    ASSERT_TRUE(maelys_mcp_process_shutdown(posix_launcher, &instance, 300u,
        2000u, &error) == MAELYS_MCP_OK);
    /* Past the graceful budget, because the polite request was ignored, and
     * nowhere near the forced one, because the kill landed. */
    elapsed = milliseconds() - started;
    ASSERT_TRUE(elapsed >= 300LL);
    ASSERT_TRUE(elapsed < 2300LL);
    free(error);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/*
 * 7. Containment failure - the case that proves the ladder is bounded at its
 * LAST rung and not merely at its middle ones. A launcher whose every wait
 * reports the child still running gets both budgets spent, destroy called
 * exactly once anyway, and MAELYS_MCP_ERR_STATE with a diagnostic naming it.
 * The contract chooses the leak over the hang, and requires it to be reported.
 */
static int case_containment_failure(void) {
    int before = open_descriptor_count();
    fake_launcher_t fake;
    fake_launcher_init(&fake, FAKE_UNKILLABLE);
    maelys_mcp_process_launcher_t launcher = fake_launcher_of(&fake);
    maelys_mcp_process_spec_t spec = {
        .executable_path = "/fake/provider",
        .max_message_bytes = 65536u,
        .grace_timeout_ms = 100u,
        .force_timeout_ms = 100u,
        .fd_layout = MAELYS_MCP_PROCESS_FD_STDIO
    };
    char *const argv[] = {(char *)"/fake/provider", NULL};
    spec.argv = argv;
    maelys_mcp_process_instance_t instance = {.protocol_fd = -1};
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_process_launch(&launcher, &spec, &instance,
        &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(instance.instance_live == 1);
    close(instance.protocol_fd);
    long long started = milliseconds();
    maelys_mcp_result_t status = maelys_mcp_process_shutdown(&launcher, &instance,
        100u, 100u, &error);
    long long elapsed = milliseconds() - started;
    ASSERT_TRUE(status == MAELYS_MCP_ERR_STATE);
    ASSERT_TRUE(error != NULL && strstr(error, "fake") != NULL);
    ASSERT_TRUE(fake.graceful_stops == 1);
    ASSERT_TRUE(fake.forced_stops == 1);
    ASSERT_TRUE(fake.wait_calls == 2);
    ASSERT_TRUE(fake.destroy_count == 1);
    ASSERT_TRUE(instance.instance_live == 0);
    /* Roughly grace + force, and emphatically not forever. */
    ASSERT_TRUE(elapsed >= 200LL);
    ASSERT_TRUE(elapsed < 10000LL);
    free(error);
    fake_launcher_clear(&fake);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/*
 * 8. Double destroy. The teardown path is driven twice and destroy still
 * reaches the launcher once, because exactly-once is tracked by instance_live
 * rather than by anything about the handle.
 */
static int case_double_destroy(void) {
    fake_launcher_t fake;
    fake_launcher_init(&fake, FAKE_SERVE_NATIVE);
    maelys_mcp_process_launcher_t launcher = fake_launcher_of(&fake);
    char *const argv[] = {(char *)"/fake/provider", NULL};
    maelys_mcp_process_spec_t spec = {
        .executable_path = "/fake/provider",
        .argv = argv,
        .max_message_bytes = 65536u,
        .grace_timeout_ms = 500u,
        .force_timeout_ms = 500u,
        .fd_layout = MAELYS_MCP_PROCESS_FD_STDIO
    };
    maelys_mcp_process_instance_t instance = {.protocol_fd = -1};
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_process_launch(&launcher, &spec, &instance,
        &error) == MAELYS_MCP_OK);
    close(instance.protocol_fd);
    ASSERT_TRUE(maelys_mcp_process_shutdown(&launcher, &instance, 500u, 500u,
        &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_process_shutdown(&launcher, &instance, 500u, 500u,
        &error) == MAELYS_MCP_OK);
    ASSERT_TRUE(fake.destroy_count == 1);
    free(error);
    fake_launcher_clear(&fake);
    return 0;
}

/*
 * 9. A NULL handle is not a destroyed handle. This is the case that fails the
 * day someone implements exactly-once against handle nullity instead of
 * against instance_live - a stateless launcher needs no per-instance state,
 * and skipping its teardown would be silent.
 */
static int case_null_handle_lifecycle(void) {
    int before = open_descriptor_count();
    fake_launcher_t fake;
    fake_launcher_init(&fake, FAKE_SERVE_NATIVE);
    fake.handle_style = HANDLE_NULL;
    maelys_mcp_process_launcher_t launcher = fake_launcher_of(&fake);
    maelys_mcp_provider_process_options_t options = native_options(5000u, 1000u);
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options, &launcher,
        &provider, &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    ASSERT_TRUE(call_directly(provider, "probe", &error) == MAELYS_MCP_OK);
    free(error);
    maelys_mcp_provider_destroy(provider);
    ASSERT_TRUE(fake.handle_seen_in_stop == 1);
    ASSERT_TRUE(fake.handle_seen_in_wait == 1);
    ASSERT_TRUE(fake.handle_seen_in_destroy == 1);
    ASSERT_TRUE(fake.last_handle == NULL);
    ASSERT_TRUE(fake.destroy_count == 1);
    fake_launcher_clear(&fake);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/*
 * 10. Invalid fd from the launcher. spawn says OK and hands over something the
 * runtime cannot use; the runtime stops the child, releases it, and fails
 * loudly with the launcher named - because "the runtime survives a broken
 * launcher" is the property a pluggable seam has to earn.
 */
static int case_invalid_fd(void) {
    int before = open_descriptor_count();
    fake_launcher_t fake;
    fake_launcher_init(&fake, FAKE_INVALID_FD);
    maelys_mcp_process_launcher_t launcher = fake_launcher_of(&fake);
    maelys_mcp_provider_process_options_t options = native_options(1000u, 200u);
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options, &launcher,
        &provider, &error) == MAELYS_MCP_ERR_PROTOCOL);
    ASSERT_TRUE(provider == NULL);
    ASSERT_TRUE(error != NULL && strstr(error, "fake") != NULL);
    ASSERT_TRUE(fake.forced_stops == 1);
    ASSERT_TRUE(fake.graceful_stops == 0);
    ASSERT_TRUE(fake.destroy_count == 1);
    free(error);
    fake_launcher_clear(&fake);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/*
 * 11. Both provider kinds through ONE launcher, which is the argument the
 * whole seam rests on: a policy enforcement point that governs native
 * providers but not proxied upstreams governs nothing. It also pins the
 * layout derivation - the proxy's spec must say STDIO because an MCP server
 * speaks stdin and stdout by specification, not because nobody set the field.
 */
static int case_both_kinds_one_launcher(void) {
    int before = open_descriptor_count();
    fake_launcher_t native_fake;
    fake_launcher_init(&native_fake, FAKE_SERVE_NATIVE);
    maelys_mcp_process_launcher_t native_launcher = fake_launcher_of(&native_fake);
    fake_launcher_t proxy_fake;
    fake_launcher_init(&proxy_fake, FAKE_SERVE_MCP);
    maelys_mcp_process_launcher_t proxy_launcher = fake_launcher_of(&proxy_fake);

    maelys_mcp_provider_process_options_t options = native_options(5000u, 500u);
    maelys_mcp_provider_t *native = NULL;
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options,
        &native_launcher, &native, &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    maelys_mcp_proxy_options_t upstream = proxy_options(5000u);
    maelys_mcp_provider_t *proxy = NULL;
    ASSERT_TRUE(maelys_mcp_provider_proxy_spawn_with_launcher(&upstream,
        &proxy_launcher, &proxy, NULL, &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    ASSERT_TRUE(call_directly(native, "probe", &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    ASSERT_TRUE(call_directly(proxy, "probe", &error) == MAELYS_MCP_OK);
    free(error);
    ASSERT_TRUE(native_fake.last_layout == MAELYS_MCP_PROCESS_FD_STDIO);
    ASSERT_TRUE(proxy_fake.last_layout == MAELYS_MCP_PROCESS_FD_STDIO);
    /* The complete vector reached the launcher, argv[0] included. */
    ASSERT_TRUE(strcmp(native_fake.last_argv0, "/fake/provider") == 0);
    ASSERT_TRUE(strcmp(proxy_fake.last_argv0, "/fake/upstream") == 0);
    maelys_mcp_provider_destroy(native);
    maelys_mcp_provider_destroy(proxy);
    ASSERT_TRUE(native_fake.spawn_count == 1 && native_fake.destroy_count == 1);
    ASSERT_TRUE(proxy_fake.spawn_count == 1 && proxy_fake.destroy_count == 1);
    fake_launcher_clear(&native_fake);
    fake_launcher_clear(&proxy_fake);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/*
 * 12. The APIs that predate the seam still behave as they did. The existing
 * suites are the real evidence for this - none of them was touched - and this
 * case adds the one assertion they cannot make: that the old entry point and
 * the new one reach the same provider through the same launcher.
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
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options,
        maelys_mcp_posix_launcher(), &provider, &error) == MAELYS_MCP_OK);
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
 * anything, and the runtime round-trips it through wait, stop and destroy
 * untouched. This is the test that fails the day someone "optimizes" the
 * handle back into a pid_t.
 */
static int case_handle_opacity(void) {
    int before = open_descriptor_count();
    fake_launcher_t fake;
    fake_launcher_init(&fake, FAKE_SERVE_NATIVE);
    fake.handle_style = HANDLE_OPAQUE;
    maelys_mcp_process_launcher_t launcher = fake_launcher_of(&fake);
    maelys_mcp_provider_process_options_t options = native_options(5000u, 1000u);
    maelys_mcp_provider_t *provider = NULL;
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_provider_spawn_with_launcher(&options, &launcher,
        &provider, &error) == MAELYS_MCP_OK);
    free(error);
    error = NULL;
    ASSERT_TRUE(call_directly(provider, "probe", &error) == MAELYS_MCP_OK);
    free(error);
    maelys_mcp_provider_destroy(provider);
    ASSERT_TRUE(fake.last_handle == FAKE_OPAQUE_HANDLE);
    ASSERT_TRUE(fake.handle_seen_in_wait && fake.handle_seen_in_stop &&
        fake.handle_seen_in_destroy);
    ASSERT_TRUE(fake.destroy_count == 1);
    fake_launcher_clear(&fake);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

/*
 * The POSIX launcher's own refusal. An executionProfile it does not implement
 * is a request for confinement it cannot provide, and starting the child
 * anyway would answer a security request with an unsandboxed process and no
 * diagnostic. Driven against the ops table directly, because no manifest key
 * carries a profile yet.
 */
static int case_posix_refuses_unknown_profile(const char *example_provider) {
    const maelys_mcp_process_launcher_t *posix_launcher = maelys_mcp_posix_launcher();
    char *const argv[] = {(char *)example_provider, (char *)"--provider", NULL};
    maelys_mcp_process_spec_t spec = {
        .executable_path = example_provider,
        .argv = argv,
        .execution_profile = "seatbelt-readonly",
        .max_message_bytes = 65536u,
        .grace_timeout_ms = 500u,
        .force_timeout_ms = 500u,
        .fd_layout = MAELYS_MCP_PROCESS_FD_STDIO
    };
    maelys_mcp_process_instance_t instance = {.protocol_fd = -1};
    char *error = NULL;
    int before = open_descriptor_count();
    ASSERT_TRUE(maelys_mcp_process_launch(posix_launcher, &spec, &instance,
        &error) == MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(error != NULL && strstr(error, "seatbelt-readonly") != NULL);
    ASSERT_TRUE(instance.instance_live == 0);
    free(error);
    error = NULL;
    ASSERT_TRUE(open_descriptor_count() == before);

    /* The two it does accept: absent, and the explicit spelling of absent. */
    spec.execution_profile = "trusted-local";
    ASSERT_TRUE(maelys_mcp_process_launch(posix_launcher, &spec, &instance,
        &error) == MAELYS_MCP_OK);
    close(instance.protocol_fd);
    ASSERT_TRUE(maelys_mcp_process_shutdown(posix_launcher, &instance, 500u, 500u,
        &error) == MAELYS_MCP_OK);
    spec.execution_profile = NULL;
    ASSERT_TRUE(maelys_mcp_process_launch(posix_launcher, &spec, &instance,
        &error) == MAELYS_MCP_OK);
    close(instance.protocol_fd);
    ASSERT_TRUE(maelys_mcp_process_shutdown(posix_launcher, &instance, 500u, 500u,
        &error) == MAELYS_MCP_OK);
    free(error);
    ASSERT_TRUE(open_descriptor_count() == before);
    return 0;
}

int main(int argc, char **argv) {
    /* example-provider, one that answers describe a second late, and one that
     * ignores SIGTERM. The fourth real child - a binary that exits at once -
     * is /usr/bin/false, named here as tests/test_process_provider.c already
     * names it rather than being built as a fixture. */
    ASSERT_TRUE(argc == 4);
    const char *example_provider = argv[1];
    const char *silent_executable = argv[2];
    const char *stubborn_executable = argv[3];
    const char *dead_executable = "/usr/bin/false";

    ASSERT_TRUE(case_spawn_failure() == 0);
    ASSERT_TRUE(case_death_before_describe(dead_executable) == 0);
    ASSERT_TRUE(case_describe_timeout(silent_executable) == 0);
    ASSERT_TRUE(case_crash_mid_call() == 0);
    ASSERT_TRUE(case_graceful_stop() == 0);
    ASSERT_TRUE(case_forced_escalation(stubborn_executable) == 0);
    ASSERT_TRUE(case_containment_failure() == 0);
    ASSERT_TRUE(case_double_destroy() == 0);
    ASSERT_TRUE(case_null_handle_lifecycle() == 0);
    ASSERT_TRUE(case_invalid_fd() == 0);
    ASSERT_TRUE(case_both_kinds_one_launcher() == 0);
    ASSERT_TRUE(case_old_apis_unchanged(example_provider) == 0);
    ASSERT_TRUE(case_handle_opacity() == 0);
    ASSERT_TRUE(case_posix_refuses_unknown_profile(example_provider) == 0);
    printf("test_process_launcher: OK\n");
    return 0;
}
