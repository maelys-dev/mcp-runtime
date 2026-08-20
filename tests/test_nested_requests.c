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
static const char *sdk_nested_provider_path;

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
            /* Named, not swallowed: the difference between "the runtime broke"
             * and "this machine could not fork another sanitized process" is
             * the whole diagnosis, and a bare assertion hides it. */
            fprintf(stderr, "provider spawn failed (%s): %s\n", provider_path,
                error ? error : "no detail");
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
    if (pipe(client->to_host) != 0 || pipe(client->from_host) != 0) {
        fprintf(stderr, "pipe failed: %s\n", strerror(errno));
        return -1;
    }
    int started = pthread_create(&client->thread, NULL, serve_main, client);
    if (started != 0) fprintf(stderr, "serve thread failed: %d\n", started);
    return started == 0 ? 0 : -1;
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

/*
 * Phase B end-to-end proof: the same happy-path and undeclared-capability
 * shapes as above, but against a provider built ON the public C SDK
 * (tests/helpers/sdk_nested_provider.c, maelys_mcp_provider_sdk_request_client)
 * rather than adversarial_provider.c's hand-rolled JSON. Nothing about the
 * host side changes between these and the pair above - same runtime, same
 * process_provider.c relay, same mrtr capability check - so any difference
 * in outcome would be the SDK's blocking helper, not the host.
 */

static int sdk_provider_nested_happy_path(void) {
    fake_client_t client;
    ASSERT_TRUE(client_start(&client, sdk_nested_provider_path, 5000u) == 0);
    ASSERT_TRUE(client_handshake(&client, "elicitation") == 0);
    ASSERT_TRUE(client_send(&client, call_request(2, "nested.ask")) == 0);

    json_t *nested = client_next(&client, 4000u);
    ASSERT_TRUE(nested != NULL);
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

static int sdk_provider_undeclared_capability_is_refused(void) {
    fake_client_t client;
    ASSERT_TRUE(client_start(&client, sdk_nested_provider_path, 5000u) == 0);
    /* No elicitation capability declared at initialize. */
    ASSERT_TRUE(client_handshake(&client, NULL) == 0);
    ASSERT_TRUE(client_send(&client, call_request(2, "nested.ask")) == 0);
    json_t *frame = client_next(&client, 4000u);
    ASSERT_TRUE(frame != NULL);
    /* The very first frame back is the response, not an elicitation request:
     * nothing was sent to a client that never offered the surface, and the
     * SDK surfaced the host's `denied` refusal cleanly rather than hanging or
     * faulting the transport. */
    ASSERT_TRUE(json_is_integer(json_object_get(frame, "id")));
    ASSERT_TRUE(json_integer_value(json_object_get(frame, "id")) == 2);
    const char *text = result_text(frame);
    ASSERT_TRUE(text && strcmp(text, "error:denied") == 0);
    json_decref(frame);
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

static int a_modern_call_never_receives_a_nested_request(void) {
    /*
     * The 2026-07-28 profile forbids separate server-to-client JSON-RPC
     * requests: a modern caller gets the resumable input_required result, no
     * matter what capabilities its _meta declares. So the same provider that
     * nests on a legacy session (nested_happy_path) must NOT produce a
     * server-to-client frame here - the first thing the client hears back
     * must be its own call's response.
     */
    fake_client_t client;
    ASSERT_TRUE(client_start(&client, nested_provider_path, 5000u) == 0);
    /* No initialize: modern sessions are stateless, era rides in _meta. */
    json_t *call = json_pack(
        "{s:s,s:i,s:s,s:{s:s,s:{},s:{s:s,s:{s:s,s:s},s:{s:{}}}}}",
        "jsonrpc", "2.0", "id", 2, "method", "tools/call",
        "params", "name", "nested.ask", "arguments",
        "_meta",
        "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
        "io.modelcontextprotocol/clientInfo",
            "name", "nested-test", "version", "1",
        "io.modelcontextprotocol/clientCapabilities", "elicitation");
    ASSERT_TRUE(call != NULL);
    ASSERT_TRUE(client_send(&client, call) == 0);

    json_t *frame = client_next(&client, 4000u);
    ASSERT_TRUE(frame != NULL);
    /* The response, not a request: it carries the call's own id and no
     * method. A nested elicitation/create arriving first is the exact
     * conformance breach this test pins. */
    ASSERT_TRUE(json_object_get(frame, "method") == NULL);
    ASSERT_TRUE(json_is_integer(json_object_get(frame, "id")));
    ASSERT_TRUE(json_integer_value(json_object_get(frame, "id")) == 2);
    json_decref(frame);
    ASSERT_TRUE(client_stop(&client) == MAELYS_MCP_OK);
    return 0;
}

/* ------------------------------------------------- the nested table itself
 *
 * Everything above drives a provider process on the far side of a pipe, which
 * is the right shape for a transport-level hazard but puts the channel's
 * nested table out of reach: how many slots it has, and which slot a request
 * lands in, are not things a fixture behind a pipe can be asked to vary. The
 * provider below runs inside this process, so the table can be built one slot
 * wide and its boundaries driven directly.
 */

/* Sentinel method: run the argument battery instead of a real round trip. */
#define ARGUMENT_SURFACE "argument-surface"

/*
 * The documented argument surface of maelys_mcp_provider_request_client,
 * checked from inside a real call, where the relay is live and the client's
 * declared capabilities are real. Returns the name of the first check that
 * did not hold, or "ok".
 */
static const char *check_argument_surface(maelys_mcp_nested_relay_t *relay) {
    json_t *result = NULL;
    char *error = NULL;
    const char *verdict = "ok";
    /*
     * "A NULL relay returns MAELYS_MCP_ERR_STATE: it is how 'this call cannot
     * nest' is expressed, so a provider must fall back rather than assume."
     */
    if (maelys_mcp_provider_request_client(NULL, "elicitation/create", NULL,
            &result, &error) != MAELYS_MCP_ERR_STATE) {
        verdict = "null-relay";
    } else if (result != NULL) {
        verdict = "null-relay-left-a-result";
    /* Not one of the three surfaces MCP defines for a server-initiated
     * request: refused before a byte is sent, and named as unsupported rather
     * than as undeclared, because those are different diagnoses. */
    } else if (maelys_mcp_provider_request_client(relay, "tools/list", NULL,
            &result, &error) != MAELYS_MCP_ERR_DENIED) {
        verdict = "unsupported-method";
    } else if (!error ||
            strcmp(error, "unsupported nested request method") != 0) {
        verdict = "unsupported-method-message";
    }
    if (strcmp(verdict, "ok") == 0) {
        /* params, when supplied, is the request's params member, so it has to
         * be an object - refused where the mistake was made rather than at
         * whatever the client makes of an array. */
        json_t *array = json_array();
        if (maelys_mcp_provider_request_client(relay, "elicitation/create",
                array, &result, &error) != MAELYS_MCP_ERR_ARGUMENT) {
            verdict = "array-params";
        }
        if (array) json_decref(array);
    }
    /* A surface this session never declared. The caller declares elicitation
     * and nothing else, so sampling is refused before it is sent. */
    if (strcmp(verdict, "ok") == 0 &&
        maelys_mcp_provider_request_client(relay, "sampling/createMessage",
            NULL, &result, &error) != MAELYS_MCP_ERR_DENIED) {
        verdict = "undeclared-capability";
    }
    /* A provider that does not want the message says so by passing no place
     * to put one, and still gets its verdict. */
    if (strcmp(verdict, "ok") == 0 &&
        maelys_mcp_provider_request_client(relay, "tools/list", NULL,
            &result, NULL) != MAELYS_MCP_ERR_DENIED) {
        verdict = "no-error-out";
    }
    /* set_error releases the previous message before writing the next, so one
     * release here covers every call above. */
    free(error);
    if (result) json_decref(result);
    return verdict;
}

/*
 * Opens the nested request its arguments name and reports the outcome as the
 * call's text, so a test reads one string instead of reaching into the
 * provider's state across threads.
 */
static maelys_mcp_result_t probe_call_nested(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_nested_relay_t *relay,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    (void)context;
    (void)out_error;
    const char *method = json_string_value(
        json_object_get(request->arguments, "method"));
    if (!method) method = "elicitation/create";
    char text[256];
    if (strcmp(method, ARGUMENT_SURFACE) == 0) {
        (void)snprintf(text, sizeof(text), "surface:%s",
            check_argument_surface(relay));
    } else {
        json_t *params = json_pack("{s:s}", "prompt", "answer me");
        json_t *answer = NULL;
        char *failure = NULL;
        maelys_mcp_result_t status = maelys_mcp_provider_request_client(
            relay, method, params, &answer, &failure);
        if (params) json_decref(params);
        if (status == MAELYS_MCP_OK) {
            const char *value = json_string_value(
                json_object_get(answer, "answer"));
            (void)snprintf(text, sizeof(text), "ok:%s", value ? value : "");
        } else {
            (void)snprintf(text, sizeof(text), "err:%s",
                failure ? failure : "");
        }
        if (answer) json_decref(answer);
        free(failure);
    }
    out_result->type = MAELYS_MCP_PROVIDER_RESULT_COMPLETE;
    out_result->content = json_pack("[{s:s,s:s}]", "type", "text",
        "text", text);
    return out_result->content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

/* Reached only on a dispatch that cannot nest, which no case here stages. */
static maelys_mcp_result_t probe_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    return probe_call_nested(context, request, NULL, out_result, out_error);
}

static maelys_mcp_runtime_t *new_in_process_runtime(
    size_t max_nested_requests,
    unsigned int nested_timeout_ms) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "nested-table-test",
        .server_version = "1",
        .max_providers = 2u,
        .max_subscriptions = 8u
    };
    maelys_mcp_runtime_t *runtime = NULL;
    if (maelys_mcp_runtime_create(&config, &runtime) != MAELYS_MCP_OK) return NULL;
    maelys_mcp_nested_config_t nested = {
        .request_timeout_ms = nested_timeout_ms,
        .max_concurrent_requests = 4u,
        .max_nested_requests = max_nested_requests
    };
    json_t *schema = json_pack("{s:s,s:{s:{s:s}}}", "type", "object",
        "properties", "method", "type", "string");
    maelys_mcp_tool_t tool = {
        .name = "nest.ask",
        .title = "Ask",
        .description = "Opens the nested client request its arguments name.",
        .input_schema = schema,
        .effect = MAELYS_MCP_EFFECT_READ
    };
    maelys_mcp_provider_config_t provider_config = {
        .name = "in-process-nest", .version = "1",
        .tools = &tool, .tool_count = 1u,
        .call = probe_call
    };
    maelys_mcp_provider_nested_handlers_t handlers = {
        .call = probe_call_nested
    };
    maelys_mcp_provider_t *provider = NULL;
    if (!schema ||
        maelys_mcp_runtime_enable_module(runtime,
            MAELYS_MCP_MODULE_TOOLS) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_enable_module(runtime,
            MAELYS_MCP_MODULE_MRTR) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_configure_nested(runtime, &nested) != MAELYS_MCP_OK ||
        maelys_mcp_provider_create(&provider_config, &provider) != MAELYS_MCP_OK ||
        maelys_mcp_provider_set_nested_handlers(provider,
            &handlers) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_add_provider(runtime, provider, NULL) != MAELYS_MCP_OK) {
        if (schema) json_decref(schema);
        if (provider) maelys_mcp_provider_destroy(provider);
        maelys_mcp_result_t destroyed = maelys_mcp_runtime_destroy(runtime);
        (void)destroyed;
        return NULL;
    }
    json_decref(schema);
    return runtime;
}

static json_t *nested_call_request(json_int_t id, const char *method) {
    return json_pack("{s:s,s:I,s:s,s:{s:s,s:{s:s}}}",
        "jsonrpc", "2.0", "id", id, "method", "tools/call",
        "params", "name", "nest.ask", "arguments", "method", method);
}

/* The same call, with the string id JSON-RPC allows and MCP permits. */
static json_t *string_id_call_request(const char *id, const char *method) {
    return json_pack("{s:s,s:s,s:s,s:{s:s,s:{s:s}}}",
        "jsonrpc", "2.0", "id", id, "method", "tools/call",
        "params", "name", "nest.ask", "arguments", "method", method);
}

typedef int (*frame_match_fn)(json_t *frame, const void *want);

static int frame_has_method(json_t *frame, const void *want) {
    json_t *method = json_object_get(frame, "method");
    return json_is_string(method) &&
        strcmp(json_string_value(method), want) == 0;
}

static int frame_has_id(json_t *frame, const void *want) {
    json_t *id = json_object_get(frame, "id");
    return json_is_integer(id) &&
        json_integer_value(id) == *(const json_int_t *)want;
}

static int frame_has_string_id(json_t *frame, const void *want) {
    json_t *id = json_object_get(frame, "id");
    return json_is_string(id) && strcmp(json_string_value(id), want) == 0;
}

/*
 * The first held or newly arrived frame the predicate accepts, removed from
 * `held` and owned by the caller. Frames that do not match are kept rather
 * than dropped: a nested request and an unrelated call's response arrive in an
 * order nothing here controls, and the frame this call did not want is the one
 * the next call is waiting for.
 */
static json_t *await_frame(
    json_t *held,
    maelys_mcp_channel_t *channel,
    frame_match_fn matches,
    const void *want,
    unsigned int timeout_ms) {
    for (;;) {
        size_t index;
        json_t *frame;
        json_array_foreach(held, index, frame) {
            if (!matches(frame, want)) continue;
            json_t *taken = json_incref(frame);
            (void)json_array_remove(held, index);
            return taken;
        }
        json_t *message = NULL;
        if (maelys_mcp_channel_next(channel, timeout_ms, &message) !=
            MAELYS_MCP_OK) {
            return NULL;
        }
        if (json_array_append_new(held, message) != 0) return NULL;
    }
}

/* Opens a session declaring exactly one client capability. */
static int open_session(maelys_mcp_channel_t *channel, const char *capability) {
    if (accept_and_release(channel, initialize_request(capability)) != 0) return -1;
    return accept_and_release(channel, initialized_notification());
}

/* How the fake client behaves once the nested request reaches it. */
typedef enum client_answer {
    ANSWER_RESULT = 0,      /* a normal result */
    ANSWER_ERROR,           /* a JSON-RPC error carrying a string message */
    ANSWER_ERROR_UNNAMED,   /* a JSON-RPC error whose message is not a string */
    ANSWER_NOTHING,         /* never answers: the deadline decides */
    ANSWER_CANCEL           /* cancels the outer call instead of answering */
} client_answer_t;

static json_t *nested_reply(json_t *nested, client_answer_t answer) {
    json_t *id = json_object_get(nested, "id");
    if (answer == ANSWER_ERROR) {
        return json_pack("{s:s,s:O,s:{s:i,s:s}}", "jsonrpc", "2.0", "id", id,
            "error", "code", -32001, "message", "the user said no");
    }
    if (answer == ANSWER_ERROR_UNNAMED) {
        /* A refusal whose `message` is not a string. The runtime must still
         * produce a message for the provider rather than reaching into it. */
        return json_pack("{s:s,s:O,s:{s:i,s:i}}", "jsonrpc", "2.0", "id", id,
            "error", "code", -32001, "message", 42);
    }
    return json_pack("{s:s,s:O,s:{s:s}}", "jsonrpc", "2.0", "id", id,
        "result", "answer", "yes");
}

/*
 * One nested round trip on a channel declaring exactly one capability, with
 * the tool's reported outcome copied out.
 *
 * The three capabilities are driven separately, each declaring only its own,
 * because that is the only arrangement in which consulting the wrong entry of
 * the method-to-capability table is visible: a session declaring all three
 * would let a request routed to the wrong capability through unnoticed.
 */
static int nested_round_trip_with(
    const char *capability,
    const char *method,
    size_t capacity,
    unsigned int nested_timeout_ms,
    client_answer_t answer,
    char *out_text,
    size_t out_size) {
    maelys_mcp_runtime_t *runtime = new_in_process_runtime(capacity,
        nested_timeout_ms);
    if (!runtime) return -1;
    maelys_mcp_channel_config_t config = {
        .max_messages = 64u, .max_bytes = 1024u * 1024u, .response_burst = 8u,
        .admission_timeout_ms = 2000u, .close_timeout_ms = 4000u
    };
    maelys_mcp_channel_t *channel = NULL;
    json_t *held = json_array();
    int failed = -1;
    if (held && maelys_mcp_channel_create(runtime, &config, &channel) ==
            MAELYS_MCP_OK &&
        open_session(channel, capability) == 0 &&
        accept_and_release(channel, nested_call_request(2, method)) == 0) {
        json_t *nested = await_frame(held, channel, frame_has_method,
            method, 4000u);
        if (!nested) {
            /* Refused before it was sent: the response is already waiting. */
            failed = 0;
        } else if (answer == ANSWER_NOTHING) {
            failed = 0;
        } else if (answer == ANSWER_CANCEL) {
            failed = accept_and_release(channel, cancel_notification(2));
        } else {
            failed = accept_and_release(channel, nested_reply(nested, answer));
        }
        if (nested) json_decref(nested);
        if (failed == 0) {
            json_int_t id = 2;
            json_t *response = await_frame(held, channel, frame_has_id, &id,
                6000u);
            const char *text = response ? result_text(response) : NULL;
            (void)snprintf(out_text, out_size, "%s", text ? text : "");
            failed = text ? 0 : -1;
            if (response) json_decref(response);
        }
    }
    if (held) json_decref(held);
    if (channel) {
        maelys_mcp_result_t destroyed = maelys_mcp_channel_destroy(channel);
        (void)destroyed;
    }
    maelys_mcp_result_t destroyed = maelys_mcp_runtime_destroy(runtime);
    return failed == 0 && destroyed == MAELYS_MCP_OK ? 0 : -1;
}

static int nested_round_trip(
    const char *capability,
    const char *method,
    char *out_text,
    size_t out_size) {
    return nested_round_trip_with(capability, method, 4u, 30000u,
        ANSWER_RESULT, out_text, out_size);
}

/*
 * All three of the client surfaces MCP defines, each over a session that
 * declared only its own capability. Two of the three had never been opened by
 * any test.
 */
static int every_nested_method_reaches_its_own_capability(void) {
    static const char *const surfaces[][2] = {
        {"elicitation", "elicitation/create"},
        {"sampling", "sampling/createMessage"},
        {"roots", "roots/list"}
    };
    for (size_t index = 0; index < 3u; ++index) {
        char text[128] = {0};
        ASSERT_TRUE(nested_round_trip(surfaces[index][0], surfaces[index][1],
            text, sizeof(text)) == 0);
        ASSERT_TRUE(strcmp(text, "ok:yes") == 0);
    }
    /* A method that is not one of the three never reaches a capability check
     * at all, and says so. */
    char text[128] = {0};
    ASSERT_TRUE(nested_round_trip("elicitation", "resources/list", text,
        sizeof(text)) == 0);
    ASSERT_TRUE(strcmp(text, "err:unsupported nested request method") == 0);
    return 0;
}

static int the_nested_argument_surface_holds(void) {
    char text[128] = {0};
    ASSERT_TRUE(nested_round_trip("elicitation", ARGUMENT_SURFACE, text,
        sizeof(text)) == 0);
    ASSERT_TRUE(strcmp(text, "surface:ok") == 0);
    return 0;
}

/*
 * A table that was never configured is not a table of zero. The public header
 * says a zero here selects MAELYS_MCP_DEFAULT_MAX_NESTED_REQUESTS, and every
 * other case in this file names a size, so nothing has ever exercised the
 * default that an embedder who configures nothing actually gets.
 */
static int an_unconfigured_table_still_admits_a_request(void) {
    char text[128] = {0};
    ASSERT_TRUE(nested_round_trip_with("elicitation", "elicitation/create",
        0u, 30000u, ANSWER_RESULT, text, sizeof(text)) == 0);
    ASSERT_TRUE(strcmp(text, "ok:yes") == 0);
    return 0;
}

/*
 * The three ways a nested request ends other than with an answer. Each has its
 * own message, and the provider is told which one happened - a deadline, a
 * cancellation and a refusal are three different things for a provider to
 * decide what to do about, and collapsing them into one failure would leave it
 * guessing.
 */
static int every_nested_failure_names_itself(void) {
    char text[128] = {0};
    /* The client answered, with a refusal. Its own message is passed through:
     * the provider is reporting to a user, and "the user said no" is the only
     * text that means anything to them. */
    ASSERT_TRUE(nested_round_trip_with("elicitation", "elicitation/create",
        4u, 30000u, ANSWER_ERROR, text, sizeof(text)) == 0);
    ASSERT_TRUE(strcmp(text, "err:the user said no") == 0);

    /* The same refusal with a `message` that is not a string. The runtime
     * substitutes its own text rather than reaching into the value. */
    ASSERT_TRUE(nested_round_trip_with("elicitation", "elicitation/create",
        4u, 30000u, ANSWER_ERROR_UNNAMED, text, sizeof(text)) == 0);
    ASSERT_TRUE(strcmp(text, "err:client refused the request") == 0);

    /* Nobody answered. */
    ASSERT_TRUE(nested_round_trip_with("elicitation", "elicitation/create",
        4u, 300u, ANSWER_NOTHING, text, sizeof(text)) == 0);
    ASSERT_TRUE(strcmp(text, "err:nested request deadline exceeded") == 0);

    /* The outer call was cancelled while the nested request was open, which
     * reaches through to the wait even though the client never named it. */
    ASSERT_TRUE(nested_round_trip_with("elicitation", "elicitation/create",
        4u, 30000u, ANSWER_CANCEL, text, sizeof(text)) == 0);
    ASSERT_TRUE(strcmp(text, "err:nested request was cancelled") == 0);
    return 0;
}

/*
 * A cancellation names one outer call, and reaches only the nested request
 * that call opened. With one entry in the table there is nothing to tell apart;
 * with two, cancelling the wrong one is a client's request answered with a
 * failure it never asked for.
 */
static int cancelling_one_call_leaves_the_other_nested_wait(void) {
    maelys_mcp_runtime_t *runtime = new_in_process_runtime(4u, 30000u);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_channel_config_t config = {
        .max_messages = 64u, .max_bytes = 1024u * 1024u, .response_burst = 8u,
        .admission_timeout_ms = 2000u, .close_timeout_ms = 4000u
    };
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, &config, &channel) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(open_session(channel, "elicitation") == 0);
    json_t *held = json_array();
    ASSERT_TRUE(held != NULL);

    /* Two calls, each holding a nested request. Waiting for each frame in
     * turn is what makes both provably open at the same time. */
    ASSERT_TRUE(accept_and_release(channel,
        nested_call_request(2, "elicitation/create")) == 0);
    json_t *first = await_frame(held, channel, frame_has_method,
        "elicitation/create", 4000u);
    ASSERT_TRUE(first != NULL);
    ASSERT_TRUE(accept_and_release(channel,
        nested_call_request(3, "elicitation/create")) == 0);
    json_t *second = await_frame(held, channel, frame_has_method,
        "elicitation/create", 4000u);
    ASSERT_TRUE(second != NULL);
    /* Two entries, two ids. */
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(first, "id")),
        json_string_value(json_object_get(second, "id"))) != 0);

    /* Cancel the first call only. */
    ASSERT_TRUE(accept_and_release(channel, cancel_notification(2)) == 0);
    json_int_t first_id = 2;
    json_t *cancelled = await_frame(held, channel, frame_has_id, &first_id,
        6000u);
    ASSERT_TRUE(cancelled != NULL);
    const char *cancelled_text = result_text(cancelled);
    ASSERT_TRUE(cancelled_text &&
        strcmp(cancelled_text, "err:nested request was cancelled") == 0);
    json_decref(cancelled);

    /* The other call is untouched, and still answerable. */
    json_int_t second_id = 3;
    json_t *early = await_frame(held, channel, frame_has_id, &second_id, 300u);
    ASSERT_TRUE(early == NULL);
    ASSERT_TRUE(accept_and_release(channel,
        nested_reply(second, ANSWER_RESULT)) == 0);
    json_t *answered = await_frame(held, channel, frame_has_id, &second_id,
        4000u);
    ASSERT_TRUE(answered != NULL);
    const char *answered_text = result_text(answered);
    ASSERT_TRUE(answered_text && strcmp(answered_text, "ok:yes") == 0);
    json_decref(answered);

    json_decref(first);
    json_decref(second);
    json_decref(held);
    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * The table's own boundary. A one-slot table has exactly one slot, and it is
 * slot zero: a scan that started at one would find no free slot at all, and a
 * scan that ran one past the end would find a free "slot" that is not in the
 * table. Neither is visible on a table with room to spare, which is every
 * other case in this file.
 */
static int a_nested_table_of_one_admits_exactly_one(void) {
    maelys_mcp_runtime_t *runtime = new_in_process_runtime(1u, 30000u);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_channel_config_t config = {
        .max_messages = 64u, .max_bytes = 1024u * 1024u, .response_burst = 8u,
        .admission_timeout_ms = 2000u, .close_timeout_ms = 4000u
    };
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, &config, &channel) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(open_session(channel, "elicitation") == 0);
    json_t *held = json_array();
    ASSERT_TRUE(held != NULL);

    /* The only slot, claimed. Waiting for the frame is what makes the second
     * call's arrival provably later than the first call's claim. */
    ASSERT_TRUE(accept_and_release(channel,
        nested_call_request(2, "elicitation/create")) == 0);
    json_t *nested = await_frame(held, channel, frame_has_method,
        "elicitation/create", 4000u);
    ASSERT_TRUE(nested != NULL);

    /* A second call finds the table full, and is told so rather than being
     * handed a slot that is not there. */
    ASSERT_TRUE(accept_and_release(channel,
        nested_call_request(3, "elicitation/create")) == 0);
    json_int_t second_id = 3;
    json_t *refused = await_frame(held, channel, frame_has_id, &second_id,
        4000u);
    ASSERT_TRUE(refused != NULL);
    const char *refusal = result_text(refused);
    ASSERT_TRUE(refusal &&
        strcmp(refusal, "err:nested request capacity reached") == 0);
    json_decref(refused);
    /* Exactly one request was ever sent to the client. */
    json_t *extra = await_frame(held, channel, frame_has_method,
        "elicitation/create", 200u);
    ASSERT_TRUE(extra == NULL);

    /*
     * A response-shaped frame whose id belongs to no entry. The demux scans
     * the whole table, matches nothing and settles nothing, and the frame
     * falls through to the dispatcher, which answers it as the stray it is.
     * This is the only shape that makes the scan run past its last entry
     * rather than stopping at a match, which is why no other case here
     * reaches the end of that loop.
     */
    json_t *stray = json_pack("{s:s,s:s,s:{s:s}}", "jsonrpc", "2.0",
        "id", "nested-99999", "result", "answer", "no");
    ASSERT_TRUE(stray != NULL);
    ASSERT_TRUE(accept_and_release(channel, stray) == 0);
    json_t *rejected = await_frame(held, channel, frame_has_string_id,
        "nested-99999", 2000u);
    ASSERT_TRUE(rejected != NULL);
    ASSERT_TRUE(json_object_get(rejected, "error") != NULL);
    json_decref(rejected);

    /* A nested id is always a string, so a response-shaped frame with an
     * integer id is not one of this table's however the table is scanned. */
    json_t *numbered = json_pack("{s:s,s:i,s:{s:s}}", "jsonrpc", "2.0",
        "id", 4242, "result", "answer", "no");
    ASSERT_TRUE(numbered != NULL);
    ASSERT_TRUE(accept_and_release(channel, numbered) == 0);
    json_int_t stray_id = 4242;
    json_t *numbered_answer = await_frame(held, channel, frame_has_id,
        &stray_id, 2000u);
    ASSERT_TRUE(numbered_answer != NULL);
    ASSERT_TRUE(json_object_get(numbered_answer, "error") != NULL);
    json_decref(numbered_answer);

    /* Answering it releases the slot and settles the call that held it - with
     * the client's answer, not the stray one that was refused above. */
    char nested_id[MAELYS_MCP_NESTED_ID_MAX] = {0};
    (void)snprintf(nested_id, sizeof(nested_id), "%s",
        json_string_value(json_object_get(nested, "id")));
    ASSERT_TRUE(accept_and_release(channel,
        nested_reply(nested, ANSWER_RESULT)) == 0);
    json_decref(nested);
    json_int_t first_id = 2;
    json_t *response = await_frame(held, channel, frame_has_id, &first_id,
        4000u);
    ASSERT_TRUE(response != NULL);
    const char *text = result_text(response);
    ASSERT_TRUE(text && strcmp(text, "ok:yes") == 0);
    json_decref(response);
    /* A matched reply is consumed by the demux rather than answered: echoing
     * an error back at a client for its own answer is a frame no client can
     * make sense of. */
    json_t *echoed = await_frame(held, channel, frame_has_string_id,
        nested_id, 300u);
    ASSERT_TRUE(echoed == NULL);

    /*
     * A request carrying a string id is still a request. It shares its id
     * shape with a nested reply and nothing else, so the demux has to tell
     * them apart by what else the frame carries rather than by the id.
     */
    ASSERT_TRUE(accept_and_release(channel,
        string_id_call_request("call-1", "resources/list")) == 0);
    json_t *answered = await_frame(held, channel, frame_has_string_id,
        "call-1", 4000u);
    ASSERT_TRUE(answered != NULL);
    const char *refused_method = result_text(answered);
    ASSERT_TRUE(refused_method &&
        strcmp(refused_method, "err:unsupported nested request method") == 0);
    json_decref(answered);

    json_decref(held);
    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

static const maelys_test_case_t cases[] = {
    {"nested happy path", nested_happy_path},
    {"a modern call never receives a nested request",
        a_modern_call_never_receives_a_nested_request},
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
    {"a C SDK provider completes a real nested round trip",
        sdk_provider_nested_happy_path},
    {"a C SDK provider surfaces the host's undeclared-capability denial",
        sdk_provider_undeclared_capability_is_refused},
    {"aborting a channel settles its nested waits",
        aborting_a_channel_settles_its_nested_waits},
    {"two calls dispatch concurrently on one channel",
        two_calls_dispatch_concurrently_on_one_channel},
    {"every nested method reaches its own capability",
        every_nested_method_reaches_its_own_capability},
    {"the nested argument surface holds", the_nested_argument_surface_holds},
    {"an unconfigured table still admits a request",
        an_unconfigured_table_still_admits_a_request},
    {"every nested failure names itself", every_nested_failure_names_itself},
    {"cancelling one call leaves the other nested wait",
        cancelling_one_call_leaves_the_other_nested_wait},
    {"a nested table of one admits exactly one",
        a_nested_table_of_one_admits_exactly_one}
};

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s <nested> <double> <dying> <legacy4> <sdk-nested>\n",
            argv[0]);
        return 1;
    }
    nested_provider_path = argv[1];
    double_provider_path = argv[2];
    dying_provider_path = argv[3];
    legacy4_provider_path = argv[4];
    sdk_nested_provider_path = argv[5];
    int failures = maelys_run_tests(cases, sizeof(cases) / sizeof(cases[0]));
    fprintf(stderr, "test_nested_requests: %s\n", failures ? "FAILED" : "OK");
    return failures;
}
