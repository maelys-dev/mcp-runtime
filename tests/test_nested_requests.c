/*
 * Nested (in-band) MRTR, end to end: a real provider process opening a real
 * server-to-client request in the middle of a call, over a real transport,
 * with a hand-rolled fake client on the other end of the pipe.
 *
 * The fake client is deliberately not the runtime's own machinery. Every
 * lifecycle hazard this file covers - a client that never answers, one that
 * cancels the outer call, a channel that goes away, a provider that dies
 * mid-wait - is a case where the two ends disagree about what is still live,
 * and a harness built out of the same code as the host would be unable to
 * stage the disagreement.
 */
#include "maelys/mcp.h"
#include "src/internal/internal.h"
#include "tests/test_support.h"

#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *nested_provider_path;
static const char *double_provider_path;
static const char *dying_provider_path;
static const char *legacy4_provider_path;

static long long milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (long long)now.tv_sec * 1000LL + (long long)now.tv_nsec / 1000000LL;
}

static maelys_mcp_runtime_t *new_runtime(
    const char *provider_path,
    unsigned int nested_timeout_ms) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "nested-test",
        .server_version = "1",
        .max_providers = 2u,
        .max_subscriptions = 8u
    };
    maelys_mcp_runtime_t *runtime = NULL;
    if (maelys_mcp_runtime_create(&config, &runtime) != MAELYS_MCP_OK) return NULL;
    maelys_mcp_nested_config_t nested = {
        .request_timeout_ms = nested_timeout_ms,
        .max_concurrent_requests = 4u,
        .max_nested_requests = 4u
    };
    if (maelys_mcp_runtime_enable_module(runtime,
            MAELYS_MCP_MODULE_TOOLS) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_enable_module(runtime,
            MAELYS_MCP_MODULE_MRTR) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_configure_nested(runtime, &nested) != MAELYS_MCP_OK) {
        /* GCC's warn_unused_result is not silenced by a cast, only by a
         * variable; see the same shape in tests/test_channels.c. */
        maelys_mcp_result_t destroyed = maelys_mcp_runtime_destroy(runtime);
        (void)destroyed;
        return NULL;
    }
    if (provider_path) {
        maelys_mcp_provider_t *provider = NULL;
        char *error = NULL;
        /*
         * A loose describe deadline, for the reason
         * tests/test_provider_perf.c gives: spawning the fixture is a liveness
         * bound this suite does not measure, and the default 5s has been seen
         * to miss under a sanitizer on a loaded machine - which fails the test
         * for the machine's reasons rather than the runtime's. The call
         * timeout stays at its default, because that one is under test.
         */
        maelys_mcp_provider_process_options_t spawn_options = {
            .executable_path = provider_path,
            .max_message_bytes = 65536u,
            .describe_timeout_ms = 30000u
        };
        if (maelys_mcp_provider_spawn_with_options(&spawn_options, &provider,
                &error) != MAELYS_MCP_OK ||
            maelys_mcp_runtime_add_provider(runtime, provider, &error) !=
                MAELYS_MCP_OK) {
            free(error);
            maelys_mcp_result_t destroyed = maelys_mcp_runtime_destroy(runtime);
            (void)destroyed;
            return NULL;
        }
        free(error);
    }
    return runtime;
}

/* ---------------------------------------------------------------- fake client
 * Two pipes rather than a socketpair: serve_stdio makes its write descriptor
 * non-blocking, and a socketpair would hand the same descriptor to the reader.
 */
typedef struct fake_client {
    maelys_mcp_runtime_t *runtime;
    int to_host[2];
    int from_host[2];
    pthread_t thread;
    maelys_mcp_result_t status;
    char buffer[262144];
    size_t length;
} fake_client_t;

static void *serve_main(void *opaque) {
    fake_client_t *client = opaque;
    client->status = maelys_mcp_runtime_serve_stdio(client->runtime,
        client->to_host[0], client->from_host[1]);
    return NULL;
}

static int client_start(
    fake_client_t *client,
    const char *provider_path,
    unsigned int nested_timeout_ms) {
    memset(client, 0, sizeof(*client));
    client->runtime = new_runtime(provider_path, nested_timeout_ms);
    if (!client->runtime) return -1;
    if (pipe(client->to_host) != 0 || pipe(client->from_host) != 0) return -1;
    return pthread_create(&client->thread, NULL, serve_main, client) == 0 ? 0 : -1;
}

static maelys_mcp_result_t client_stop(fake_client_t *client) {
    close(client->to_host[1]);
    (void)pthread_join(client->thread, NULL);
    close(client->to_host[0]);
    close(client->from_host[0]);
    close(client->from_host[1]);
    maelys_mcp_result_t destroyed = maelys_mcp_runtime_destroy(client->runtime);
    return client->status != MAELYS_MCP_OK ? client->status : destroyed;
}

static int client_send(fake_client_t *client, json_t *message) {
    char *encoded = json_dumps(message, JSON_COMPACT);
    json_decref(message);
    if (!encoded) return -1;
    size_t length = strlen(encoded);
    encoded[length] = '\n';
    ssize_t written = write(client->to_host[1], encoded, length + 1u);
    free(encoded);
    return written == (ssize_t)(length + 1u) ? 0 : -1;
}

/* One frame, or NULL if none arrived within the deadline. */
static json_t *client_next(fake_client_t *client, unsigned int timeout_ms) {
    long long deadline = milliseconds() + (long long)timeout_ms;
    for (;;) {
        char *newline = memchr(client->buffer, '\n', client->length);
        if (newline) {
            size_t line = (size_t)(newline - client->buffer);
            json_t *value = json_loadb(client->buffer, line, 0, NULL);
            memmove(client->buffer, newline + 1, client->length - line - 1u);
            client->length -= line + 1u;
            return value;
        }
        long long remaining = deadline - milliseconds();
        if (remaining <= 0) return NULL;
        struct pollfd descriptor = {
            .fd = client->from_host[0], .events = POLLIN
        };
        int ready = poll(&descriptor, 1u, (int)remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return NULL;
        ssize_t bytes = read(client->from_host[0],
            client->buffer + client->length,
            sizeof(client->buffer) - client->length);
        if (bytes <= 0) return NULL;
        client->length += (size_t)bytes;
    }
}

/* The frame whose `id` matches, skipping whatever else the host sent first -
 * a nested request and a response are both legal here, in either order. */
static json_t *client_next_with_id(
    fake_client_t *client,
    json_int_t id,
    unsigned int timeout_ms) {
    long long deadline = milliseconds() + (long long)timeout_ms;
    for (;;) {
        long long remaining = deadline - milliseconds();
        if (remaining <= 0) return NULL;
        json_t *frame = client_next(client, (unsigned int)remaining);
        if (!frame) return NULL;
        json_t *value = json_object_get(frame, "id");
        if (json_is_integer(value) && json_integer_value(value) == id) {
            return frame;
        }
        json_decref(frame);
    }
}

static json_t *initialize_request(const char *capability) {
    json_t *capabilities = json_object();
    if (capability) {
        (void)json_object_set_new(capabilities, capability, json_object());
    }
    return json_pack("{s:s,s:i,s:s,s:{s:s,s:o,s:{s:s,s:s}}}",
        "jsonrpc", "2.0", "id", 1, "method", "initialize",
        "params", "protocolVersion", MAELYS_MCP_PROTOCOL_LEGACY,
        "capabilities", capabilities,
        "clientInfo", "name", "nested-test", "version", "1");
}

static json_t *initialized_notification(void) {
    return json_pack("{s:s,s:s}", "jsonrpc", "2.0",
        "method", "notifications/initialized");
}

static json_t *call_request(json_int_t id, const char *tool) {
    return json_pack("{s:s,s:I,s:s,s:{s:s,s:{}}}",
        "jsonrpc", "2.0", "id", id, "method", "tools/call",
        "params", "name", tool, "arguments");
}

static json_t *cancel_notification(json_int_t id) {
    return json_pack("{s:s,s:s,s:{s:I}}", "jsonrpc", "2.0",
        "method", "notifications/cancelled", "params", "requestId", id);
}

/* Opens the legacy session declaring `capability` (NULL declares none). */
static int client_handshake(fake_client_t *client, const char *capability) {
    if (client_send(client, initialize_request(capability)) != 0) return -1;
    json_t *response = client_next(client, 4000u);
    if (!response) return -1;
    json_decref(response);
    return client_send(client, initialized_notification());
}

/* The first text block of a tools/call result. */
static const char *result_text(json_t *response) {
    json_t *result = json_object_get(response, "result");
    json_t *content = json_is_object(result) ?
        json_object_get(result, "content") : NULL;
    json_t *block = json_is_array(content) ? json_array_get(content, 0) : NULL;
    json_t *text = json_is_object(block) ? json_object_get(block, "text") : NULL;
    return json_is_string(text) ? json_string_value(text) : NULL;
}

static int result_is_error(json_t *response) {
    json_t *result = json_object_get(response, "result");
    return json_is_object(result) &&
        json_is_true(json_object_get(result, "isError"));
}

/* ------------------------------------------------------------------- cases */

static int nested_happy_path(void) {
    fake_client_t client;
    ASSERT_TRUE(client_start(&client, nested_provider_path, 5000u) == 0);
    ASSERT_TRUE(client_handshake(&client, "elicitation") == 0);
    ASSERT_TRUE(client_send(&client, call_request(2, "nested.ask")) == 0);

    json_t *nested = client_next(&client, 4000u);
    ASSERT_TRUE(nested != NULL);
    /* A real server-to-client request: its own host-generated id, the method
     * the provider named, and the params it supplied. */
    ASSERT_TRUE(json_is_string(json_object_get(nested, "id")));
    ASSERT_TRUE(strncmp(json_string_value(json_object_get(nested, "id")),
        MAELYS_MCP_NESTED_ID_PREFIX, strlen(MAELYS_MCP_NESTED_ID_PREFIX)) == 0);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(nested, "method")),
        "elicitation/create") == 0);
    ASSERT_TRUE(json_is_object(json_object_get(nested, "params")));

    json_t *reply = json_pack("{s:s,s:O,s:{s:s}}", "jsonrpc", "2.0",
        "id", json_object_get(nested, "id"), "result", "answer", "yes");
    json_decref(nested);
    ASSERT_TRUE(client_send(&client, reply) == 0);

    json_t *response = client_next_with_id(&client, 2, 4000u);
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(!result_is_error(response));
    const char *text = result_text(response);
    ASSERT_TRUE(text && strcmp(text, "yes") == 0);
    json_decref(response);
    ASSERT_TRUE(client_stop(&client) == MAELYS_MCP_OK);
    return 0;
}

static int nested_reply_travels_with_the_call(void) {
    /*
     * Two calls in flight on one channel at once, answered out of order. The
     * point is the correlation table: the reply the client sends second must
     * reach the call that asked first, and both calls must come back with
     * their own answer rather than each other's.
     */
    fake_client_t client;
    ASSERT_TRUE(client_start(&client, nested_provider_path, 5000u) == 0);
    ASSERT_TRUE(client_handshake(&client, "elicitation") == 0);
    ASSERT_TRUE(client_send(&client, call_request(2, "nested.ask")) == 0);
    json_t *first = client_next(&client, 4000u);
    ASSERT_TRUE(first != NULL);
    ASSERT_TRUE(client_send(&client, json_pack("{s:s,s:O,s:{s:s}}",
        "jsonrpc", "2.0", "id", json_object_get(first, "id"),
        "result", "answer", "first")) == 0);
    json_decref(first);
    json_t *response = client_next_with_id(&client, 2, 4000u);
    ASSERT_TRUE(response != NULL);
    const char *text = result_text(response);
    ASSERT_TRUE(text && strcmp(text, "first") == 0);
    json_decref(response);

    ASSERT_TRUE(client_send(&client, call_request(3, "nested.ask")) == 0);
    json_t *second = client_next(&client, 4000u);
    ASSERT_TRUE(second != NULL);
    /* The counter advanced: a second nested request never reuses the id the
     * first one was answered on. */
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(second, "id")),
        MAELYS_MCP_NESTED_ID_PREFIX "1") != 0);
    ASSERT_TRUE(client_send(&client, json_pack("{s:s,s:O,s:{s:s}}",
        "jsonrpc", "2.0", "id", json_object_get(second, "id"),
        "result", "answer", "second")) == 0);
    json_decref(second);
    response = client_next_with_id(&client, 3, 4000u);
    ASSERT_TRUE(response != NULL);
    text = result_text(response);
    ASSERT_TRUE(text && strcmp(text, "second") == 0);
    json_decref(response);
    ASSERT_TRUE(client_stop(&client) == MAELYS_MCP_OK);
    return 0;
}

static int nested_request_times_out(void) {
    fake_client_t client;
    ASSERT_TRUE(client_start(&client, nested_provider_path, 400u) == 0);
    ASSERT_TRUE(client_handshake(&client, "elicitation") == 0);
    ASSERT_TRUE(client_send(&client, call_request(2, "nested.ask")) == 0);
    json_t *nested = client_next(&client, 4000u);
    ASSERT_TRUE(nested != NULL);
    json_decref(nested);
    /* Deliberately never answered. */
    long long started = milliseconds();
    json_t *response = client_next_with_id(&client, 2, 4000u);
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(milliseconds() - started < 3000);
    /* The provider is told the client timed out and reports that back; the
     * call fails rather than hanging or being silently completed. */
    const char *text = result_text(response);
    ASSERT_TRUE(text && strcmp(text, "error:timeout") == 0);
    json_decref(response);
    ASSERT_TRUE(client_stop(&client) == MAELYS_MCP_OK);
    return 0;
}

static int cancelling_the_outer_call_reaches_the_nested_wait(void) {
    /* The nested deadline is 30 seconds here on purpose: if the cancel did not
     * reach through, this test would not fail, it would hang - so the elapsed
     * assertion is the real one. */
    fake_client_t client;
    ASSERT_TRUE(client_start(&client, nested_provider_path, 30000u) == 0);
    ASSERT_TRUE(client_handshake(&client, "elicitation") == 0);
    ASSERT_TRUE(client_send(&client, call_request(2, "nested.ask")) == 0);
    json_t *nested = client_next(&client, 4000u);
    ASSERT_TRUE(nested != NULL);
    json_decref(nested);
    long long started = milliseconds();
    ASSERT_TRUE(client_send(&client, cancel_notification(2)) == 0);
    json_t *response = client_next_with_id(&client, 2, 5000u);
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(milliseconds() - started < 4000);
    const char *text = result_text(response);
    ASSERT_TRUE(text && strcmp(text, "error:cancelled") == 0);
    json_decref(response);
    ASSERT_TRUE(client_stop(&client) == MAELYS_MCP_OK);
    return 0;
}

static int provider_death_mid_nested_wait_frees_the_worker(void) {
    fake_client_t client;
    ASSERT_TRUE(client_start(&client, dying_provider_path, 30000u) == 0);
    ASSERT_TRUE(client_handshake(&client, "elicitation") == 0);
    long long started = milliseconds();
    ASSERT_TRUE(client_send(&client, call_request(2, "nested.ask")) == 0);
    json_t *response = client_next_with_id(&client, 2, 5000u);
    ASSERT_TRUE(response != NULL);
    /* Not after the nested deadline: the provider's death cancels the wait. */
    ASSERT_TRUE(milliseconds() - started < 4000);
    ASSERT_TRUE(result_is_error(response));
    json_decref(response);
    ASSERT_TRUE(client_stop(&client) != MAELYS_MCP_ERR_TIMEOUT);
    return 0;
}

static int a_second_nested_request_is_fatal(void) {
    fake_client_t client;
    ASSERT_TRUE(client_start(&client, double_provider_path, 30000u) == 0);
    ASSERT_TRUE(client_handshake(&client, "elicitation") == 0);
    long long started = milliseconds();
    ASSERT_TRUE(client_send(&client, call_request(2, "nested.ask")) == 0);
    json_t *response = client_next_with_id(&client, 2, 5000u);
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(milliseconds() - started < 4000);
    ASSERT_TRUE(result_is_error(response));
    json_decref(response);
    /* The transport is dead, not merely this call: a provider that lost track
     * of which reply answers which request cannot be trusted with the next
     * one either. */
    ASSERT_TRUE(client_send(&client, call_request(3, "nested.ask")) == 0);
    json_t *second = client_next_with_id(&client, 3, 5000u);
    ASSERT_TRUE(second != NULL);
    ASSERT_TRUE(result_is_error(second));
    json_decref(second);
    ASSERT_TRUE(client_stop(&client) != MAELYS_MCP_ERR_TIMEOUT);
    return 0;
}

static int an_undeclared_capability_is_refused_before_it_is_sent(void) {
    fake_client_t client;
    ASSERT_TRUE(client_start(&client, nested_provider_path, 5000u) == 0);
    /* No elicitation capability declared at initialize. */
    ASSERT_TRUE(client_handshake(&client, NULL) == 0);
    ASSERT_TRUE(client_send(&client, call_request(2, "nested.ask")) == 0);
    json_t *frame = client_next(&client, 4000u);
    ASSERT_TRUE(frame != NULL);
    /* The very first frame back is the response, not an elicitation request:
     * nothing was sent to a client that never offered the surface. */
    ASSERT_TRUE(json_is_integer(json_object_get(frame, "id")));
    ASSERT_TRUE(json_integer_value(json_object_get(frame, "id")) == 2);
    const char *text = result_text(frame);
    ASSERT_TRUE(text && strcmp(text, "error:denied") == 0);
    json_decref(frame);
    ASSERT_TRUE(client_stop(&client) == MAELYS_MCP_OK);
    return 0;
}

static int a_version_four_provider_still_works(void) {
    /*
     * The regression the /5 bump could have caused: /4 is neither the floor
     * nor the current version, so a host that checked "floor or newest" would
     * reject every provider released against 0.13.0.
     */
    fake_client_t client;
    ASSERT_TRUE(client_start(&client, legacy4_provider_path, 5000u) == 0);
    ASSERT_TRUE(client_handshake(&client, "elicitation") == 0);
    ASSERT_TRUE(client_send(&client, call_request(2, "nested.ask")) == 0);
    json_t *response = client_next_with_id(&client, 2, 4000u);
    ASSERT_TRUE(response != NULL);
    const char *text = result_text(response);
    ASSERT_TRUE(text && strcmp(text, "legacy4-ok") == 0);
    json_decref(response);
    ASSERT_TRUE(client_stop(&client) == MAELYS_MCP_OK);
    return 0;
}

/* ------------------------------------------- channel-level lifecycle hazards
 * These drive maelys_mcp_channel_accept directly rather than through stdio,
 * because they need to do something to the channel - abort it, or run two
 * calls through it at once - that no client can ask a transport for.
 */

static json_t *legacy_open(void) {
    return initialize_request("elicitation");
}

static int accept_and_release(maelys_mcp_channel_t *channel, json_t *request) {
    maelys_mcp_result_t status = maelys_mcp_channel_accept(channel, request);
    json_decref(request);
    return status == MAELYS_MCP_OK ? 0 : -1;
}

static int aborting_a_channel_settles_its_nested_waits(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(nested_provider_path, 30000u);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_channel_config_t config = {
        .max_messages = 64u, .max_bytes = 1024u * 1024u, .response_burst = 8u,
        .admission_timeout_ms = 2000u, .close_timeout_ms = 2000u
    };
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, &config, &channel) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(accept_and_release(channel, legacy_open()) == 0);
    ASSERT_TRUE(accept_and_release(channel, initialized_notification()) == 0);
    ASSERT_TRUE(accept_and_release(channel, call_request(2, "nested.ask")) == 0);
    /* Drain until the nested request appears, so the worker is provably
     * inside the wait when the channel is pulled out from under it. */
    int saw_nested = 0;
    for (int attempt = 0; attempt < 8 && !saw_nested; ++attempt) {
        json_t *message = NULL;
        if (maelys_mcp_channel_next(channel, 2000u, &message) != MAELYS_MCP_OK) {
            break;
        }
        json_t *method = json_object_get(message, "method");
        saw_nested = json_is_string(method) &&
            strcmp(json_string_value(method), "elicitation/create") == 0;
        json_decref(message);
    }
    ASSERT_TRUE(saw_nested);
    long long started = milliseconds();
    maelys_mcp_channel_abort(channel);
    /* destroy waits for every in-flight operation, so it cannot return until
     * the worker has left its nested wait. */
    (void)maelys_mcp_channel_destroy(channel);
    ASSERT_TRUE(milliseconds() - started < 5000);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/* --------------------------------------------------- concurrent dispatch
 * The first test in this repository where two requests genuinely dispatch at
 * the same time on ONE channel. The provider blocks until both calls have
 * arrived, so the overlap is guaranteed rather than hoped for, and the whole
 * dispatch path - runtime, tools module, middleware chain - runs twice over
 * the same channel simultaneously. Its real job is to give TSan something to
 * look at.
 */
typedef struct rendezvous {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    size_t arrived;
    size_t expected;
} rendezvous_t;

static rendezvous_t concurrent_rendezvous;
static atomic_int concurrent_wrap_count;
/* Set only by a call that actually saw the other one already inside the
 * provider. A serial runtime never sets it. */
static atomic_int concurrent_overlap;

/*
 * Waits for its peer, but not forever: a runtime that dispatched serially must
 * make this test fail rather than deadlock it, or the test reports nothing at
 * all when the property it exists for is gone.
 */
static maelys_mcp_result_t concurrent_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    rendezvous_t *rendezvous = context;
    (void)out_error;
    long long deadline = milliseconds() + 2000LL;
    pthread_mutex_lock(&rendezvous->mutex);
    /*
     * `arrived` counts callers currently *inside*, not callers seen so far.
     * A running total would reach two even when the two calls ran one after
     * the other, which is precisely the state this test has to be able to
     * tell apart from real overlap.
     */
    rendezvous->arrived++;
    if (rendezvous->arrived >= rendezvous->expected) {
        atomic_store(&concurrent_overlap, 1);
        pthread_cond_broadcast(&rendezvous->ready);
    }
    while (rendezvous->arrived < rendezvous->expected &&
        milliseconds() < deadline) {
        struct timespec wait = {.tv_sec = 0, .tv_nsec = 20000000L};
        pthread_mutex_unlock(&rendezvous->mutex);
        while (nanosleep(&wait, &wait) != 0 && errno == EINTR) {}
        pthread_mutex_lock(&rendezvous->mutex);
    }
    rendezvous->arrived--;
    pthread_mutex_unlock(&rendezvous->mutex);
    out_result->type = MAELYS_MCP_PROVIDER_RESULT_COMPLETE;
    out_result->structured_content = json_pack("{s:s}", "echo",
        json_string_value(json_object_get(request->arguments, "tag")));
    return out_result->structured_content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static maelys_mcp_authorize_decision_t concurrent_authorize(
    void *context,
    const maelys_mcp_authorize_context_t *request) {
    (void)context;
    (void)request;
    return MAELYS_MCP_AUTHORIZE_ALLOW;
}

static maelys_mcp_result_t concurrent_wrap_sink(
    void *context,
    const maelys_mcp_wrap_sink_context_t *request,
    maelys_mcp_sink_wrapper_t *wrapper) {
    (void)context;
    (void)request;
    (void)wrapper;
    atomic_fetch_add(&concurrent_wrap_count, 1);
    return MAELYS_MCP_OK;
}

static int two_calls_dispatch_concurrently_on_one_channel(void) {
    maelys_mcp_runtime_t *runtime = new_runtime(NULL, 5000u);
    ASSERT_TRUE(runtime != NULL);
    ASSERT_TRUE(pthread_mutex_init(&concurrent_rendezvous.mutex, NULL) == 0);
    ASSERT_TRUE(pthread_cond_init(&concurrent_rendezvous.ready, NULL) == 0);
    concurrent_rendezvous.arrived = 0u;
    concurrent_rendezvous.expected = 2u;
    atomic_init(&concurrent_wrap_count, 0);
    atomic_init(&concurrent_overlap, 0);

    json_t *schema = json_pack("{s:s,s:{s:{s:s}}}", "type", "object",
        "properties", "tag", "type", "string");
    ASSERT_TRUE(schema != NULL);
    maelys_mcp_tool_t tool = {
        .name = "slow.echo",
        .title = "Slow echo",
        .description = "Blocks until every concurrent caller has arrived.",
        .input_schema = schema,
        .effect = MAELYS_MCP_EFFECT_READ
    };
    maelys_mcp_provider_config_t provider_config = {
        .name = "rendezvous", .version = "1",
        .tools = &tool, .tool_count = 1u,
        .call = concurrent_call,
        .context = &concurrent_rendezvous
    };
    maelys_mcp_provider_t *provider = NULL;
    ASSERT_TRUE(maelys_mcp_provider_create(&provider_config, &provider) ==
        MAELYS_MCP_OK);
    json_decref(schema);
    ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, provider, NULL) ==
        MAELYS_MCP_OK);
    /* The chain is on the dispatch path now that two dispatches can overlap,
     * so it is exercised here rather than left to the single-threaded suite. */
    maelys_mcp_middleware_t middleware = {
        .name = "concurrent-probe",
        .on_authorize = concurrent_authorize,
        .wrap_sink = concurrent_wrap_sink
    };
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
        MAELYS_MCP_OK);

    maelys_mcp_channel_config_t config = {
        .max_messages = 64u, .max_bytes = 1024u * 1024u, .response_burst = 8u,
        .admission_timeout_ms = 2000u, .close_timeout_ms = 4000u
    };
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, &config, &channel) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(accept_and_release(channel, legacy_open()) == 0);
    ASSERT_TRUE(accept_and_release(channel, initialized_notification()) == 0);
    json_t *initialize_response = NULL;
    ASSERT_TRUE(maelys_mcp_channel_next(channel, 2000u, &initialize_response) ==
        MAELYS_MCP_OK);
    json_decref(initialize_response);

    for (json_int_t id = 2; id <= 3; ++id) {
        json_t *request = json_pack("{s:s,s:I,s:s,s:{s:s,s:{s:s}}}",
            "jsonrpc", "2.0", "id", id, "method", "tools/call",
            "params", "name", "slow.echo", "arguments", "tag",
            id == 2 ? "first" : "second");
        ASSERT_TRUE(request != NULL);
        ASSERT_TRUE(accept_and_release(channel, request) == 0);
    }
    /* Both must be answered, and both must have been inside the provider at
     * the same time. A runtime that dispatched these serially still answers
     * both - it just never overlaps - which is why the overlap flag, not the
     * pair of answers, is what this test is really asserting. */
    int seen_first = 0;
    int seen_second = 0;
    for (int attempt = 0; attempt < 4 && !(seen_first && seen_second); ++attempt) {
        json_t *message = NULL;
        ASSERT_TRUE(maelys_mcp_channel_next(channel, 4000u, &message) ==
            MAELYS_MCP_OK);
        json_t *result = json_object_get(message, "result");
        json_t *structured = json_is_object(result) ?
            json_object_get(result, "structuredContent") : NULL;
        json_t *echo = json_is_object(structured) ?
            json_object_get(structured, "echo") : NULL;
        if (json_is_string(echo)) {
            if (strcmp(json_string_value(echo), "first") == 0) seen_first = 1;
            if (strcmp(json_string_value(echo), "second") == 0) seen_second = 1;
        }
        json_decref(message);
    }
    ASSERT_TRUE(seen_first && seen_second);
    ASSERT_TRUE(atomic_load(&concurrent_overlap) == 1);
    ASSERT_TRUE(atomic_load(&concurrent_wrap_count) >= 2);
    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    pthread_cond_destroy(&concurrent_rendezvous.ready);
    pthread_mutex_destroy(&concurrent_rendezvous.mutex);
    return 0;
}

static const maelys_test_case_t cases[] = {
    {"nested happy path", nested_happy_path},
    {"nested reply travels with its call", nested_reply_travels_with_the_call},
    {"nested request times out", nested_request_times_out},
    {"cancelling the outer call reaches the nested wait",
        cancelling_the_outer_call_reaches_the_nested_wait},
    {"provider death mid nested wait frees the worker",
        provider_death_mid_nested_wait_frees_the_worker},
    {"a second nested request is fatal", a_second_nested_request_is_fatal},
    {"an undeclared capability is refused before it is sent",
        an_undeclared_capability_is_refused_before_it_is_sent},
    {"a version four provider still works", a_version_four_provider_still_works},
    {"aborting a channel settles its nested waits",
        aborting_a_channel_settles_its_nested_waits},
    {"two calls dispatch concurrently on one channel",
        two_calls_dispatch_concurrently_on_one_channel}
};

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s <nested> <double> <dying> <legacy4>\n", argv[0]);
        return 1;
    }
    nested_provider_path = argv[1];
    double_provider_path = argv[2];
    dying_provider_path = argv[3];
    legacy4_provider_path = argv[4];
    int failures = maelys_run_tests(cases, sizeof(cases) / sizeof(cases[0]));
    fprintf(stderr, "test_nested_requests: %s\n", failures ? "FAILED" : "OK");
    return failures;
}
