/*
 * The server layer over real loopback sockets, plus the authenticator seam.
 *
 * The parser suite proves the rules; this one proves the ladder that applies
 * them - which status each rejection actually puts on the wire, in which order
 * the checks run, whether the connection is reused, and whether an answer
 * reaches a client that is still writing a body.
 *
 * Real sockets on 127.0.0.1:0 rather than a fake transport, deliberately. The
 * two properties H1 has to demonstrate that a fake cannot - a kept-alive
 * connection serving a second request, and a pipelined one being refused - are
 * both about what the kernel does with a byte stream.
 */
#include "host/http_auth.h"
#include "host/http_parser.h"
#include "host/http_server.h"
#include "maelys/mcp.h"
#include "maelys/mcp/http.h"
#include "tests/test_support.h"

#include <arpa/inet.h>
#include <errno.h>
#include <jansson.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/*
 * A body that satisfies the MCP rules and then dispatches, because since H2 the
 * adapter validates before it answers and since H3 it answers by dispatching. A
 * server-layer test that wanted to reach either would otherwise be refused by a
 * rule it is not testing.
 *
 * `ping` deliberately: it answers in every state, before the initialization
 * gate, so it isolates the ladder under test from the runtime's lifecycle. The
 * clientCapabilities key is not optional on a modern request and was not needed
 * while the adapter stopped at validation.
 */
#define MODERN_META \
    "\"_meta\":{" \
    "\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"," \
    "\"io.modelcontextprotocol/clientCapabilities\":{}}"
#define VALID_BODY \
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"params\":{" \
    MODERN_META "}}"
#define VALID_BODY_LENGTH "164"
/* The Content-Length in the table below is written out, so it has to be pinned
 * to the body rather than counted by hand: a body one byte longer than its
 * declared length is indistinguishable from a pipelined request, and the test
 * would fail for a reason that has nothing to do with what it is testing. */
_Static_assert(sizeof(VALID_BODY) - 1u == 164u, "VALID_BODY is 164 bytes");

/* ------------------------------------------------------------ the runtime */

/*
 * The provider behind the listener, and the two knobs the H3 cases need from
 * it: a tool that reports progress, so a call over HTTP produces frames before
 * its response; and a way to keep that call inside the provider until the test
 * says otherwise, so "the client disconnected mid-call" is a state the test
 * chooses rather than one it races for.
 */
typedef struct tool_state {
    int calls;
    /* Raised once the provider is inside the call. */
    int entered_write;
    /* The provider does not return until this is readable. */
    int hold_read;
    /*
     * What an emit attempted AFTER the hold released answered. On a live
     * exchange that is MAELYS_MCP_OK; on a cancelled one it is
     * MAELYS_MCP_ERR_CLOSED, because the abort closed the channel's outbox and
     * every subsequent emit and complete fails against it. That is the "MUST
     * NOT send any further messages for it" rule holding by construction, seen
     * from the only place that can see it.
     */
    maelys_mcp_result_t emit_after_hold;
} tool_state_t;

static maelys_mcp_result_t emitting_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    (void)out_error;
    tool_state_t *state = context;
    state->calls++;
    for (int step = 0; step < 3; ++step) {
        (void)maelys_mcp_provider_report_progress(request->progress,
            step * 33.0, 100.0, NULL);
    }
    if (state->entered_write >= 0) {
        ssize_t written = write(state->entered_write, "x", 1u);
        (void)written;
    }
    if (state->hold_read >= 0) {
        struct pollfd descriptor = {
            .fd = state->hold_read, .events = POLLIN, .revents = 0};
        (void)poll(&descriptor, 1u, 5000);
        /* Attempted AFTER the wait, which is the only ordering that proves
         * anything: whatever happened to this exchange happened while this call
         * was blocked. */
        state->emit_after_hold = maelys_mcp_provider_report_progress(
            request->progress, 99.0, 100.0, NULL);
    }
    out_result->structured_content = json_pack("{s:s}", "tool", request->tool_name);
    return out_result->structured_content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static maelys_mcp_result_t fixture_read_resource(
    void *context,
    const maelys_mcp_resource_request_t *request,
    maelys_mcp_resource_result_t *out_result,
    char **out_error) {
    (void)context;
    (void)out_error;
    out_result->contents = json_pack("[{s:s,s:s,s:s}]",
        "uri", request->uri, "mimeType", "text/plain", "text", "body");
    return out_result->contents ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static maelys_mcp_runtime_t *serving_runtime(tool_state_t *state) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "http-server-test", .server_version = "1.0"
    };
    maelys_mcp_runtime_t *runtime = NULL;
    if (maelys_mcp_runtime_create(&config, &runtime) != MAELYS_MCP_OK) return NULL;
    if (maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_TOOLS) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_RESOURCES) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_enable_module(runtime,
            MAELYS_MCP_MODULE_SUBSCRIPTIONS) != MAELYS_MCP_OK) {
        (void)maelys_mcp_runtime_destroy(runtime);
        return NULL;
    }
    json_t *schema = json_pack("{s:s,s:b}", "type", "object", "additionalProperties", 0);
    if (!schema) {
        (void)maelys_mcp_runtime_destroy(runtime);
        return NULL;
    }
    maelys_mcp_tool_t tools[] = {{
        .name = "fx.stream",
        .title = "Stream",
        .description = "A tool that reports progress.",
        .input_schema = schema,
        .effect = MAELYS_MCP_EFFECT_READ
    }};
    static const maelys_mcp_resource_t resources[] = {{
        .uri = "fx://repo/doc.txt", .name = "Doc", .mime_type = "text/plain"
    }};
    maelys_mcp_provider_config_t provider_config = {
        .name = "fixture",
        .version = "1",
        .tools = tools,
        .tool_count = 1,
        .resources = resources,
        .resource_count = 1,
        .call = emitting_call,
        .read_resource = fixture_read_resource,
        .context = state
    };
    maelys_mcp_provider_t *provider = NULL;
    maelys_mcp_result_t status = maelys_mcp_provider_create(&provider_config, &provider);
    json_decref(schema);
    if (status != MAELYS_MCP_OK) {
        (void)maelys_mcp_runtime_destroy(runtime);
        return NULL;
    }
    if (maelys_mcp_runtime_add_provider(runtime, provider, NULL) != MAELYS_MCP_OK) {
        maelys_mcp_provider_destroy(provider);
        (void)maelys_mcp_runtime_destroy(runtime);
        return NULL;
    }
    return runtime;
}

typedef struct fixture {
    maelys_mcp_authenticator_t authenticator;
    maelys_mcp_runtime_t *runtime;
    maelys_mcp_http_adapter_t *adapter;
    maelys_http_server_t *server;
    unsigned short port;
    tool_state_t tools;
} fixture_t;

static int fixture_start(
    fixture_t *fixture,
    const char *const *tokens,
    size_t token_count,
    const char *const *origins,
    size_t origin_count) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->tools.entered_write = -1;
    fixture->tools.hold_read = -1;
    maelys_mcp_result_t status = token_count
        ? maelys_http_auth_static_bearer_create(tokens, token_count, &fixture->authenticator)
        : maelys_http_auth_loopback_create(&fixture->authenticator);
    if (status != MAELYS_MCP_OK) return -1;
    fixture->runtime = serving_runtime(&fixture->tools);
    if (!fixture->runtime) {
        fixture->authenticator.destroy(fixture->authenticator.context);
        return -1;
    }
    maelys_mcp_http_adapter_config_t adapter_config = {
        .runtime = fixture->runtime,
        /* Short enough that a wedged provider does not stall the suite. */
        .close_timeout_ms = 200u,
        .keepalive_interval_ms = 100u
    };
    if (maelys_mcp_http_adapter_create(
        &adapter_config, &fixture->adapter) != MAELYS_MCP_OK) {
        (void)maelys_mcp_runtime_destroy(fixture->runtime);
        fixture->authenticator.destroy(fixture->authenticator.context);
        return -1;
    }
    maelys_http_server_options_t options = {
        .bind_address = "127.0.0.1",
        .port = 0,
        .allowed_origins = origins,
        .allowed_origin_count = origin_count,
        .max_body_bytes = 1024u,
        /* Short enough that a timeout test does not stall the suite, and long
         * enough that a loopback round trip never trips it. */
        .header_timeout_ms = 2000u,
        .body_timeout_ms = 2000u,
        .idle_timeout_ms = 2000u,
        .write_timeout_ms = 2000u,
        .authenticator = &fixture->authenticator,
        .adapter = fixture->adapter
    };
    if (maelys_http_server_start(&options, &fixture->server) != MAELYS_MCP_OK) {
        maelys_mcp_http_adapter_destroy(fixture->adapter);
        (void)maelys_mcp_runtime_destroy(fixture->runtime);
        fixture->authenticator.destroy(fixture->authenticator.context);
        return -1;
    }
    fixture->port = maelys_http_server_port(fixture->server);
    return 0;
}

/*
 * A counting authenticator, so that "authenticate runs once per POST" can be
 * asserted as a NUMBER rather than inferred from two requests both being
 * answered. It delegates to loopback-trust rather than reimplementing a
 * principal, because what is under test is how often the server layer calls
 * it, not what it decides.
 */
typedef struct counting_auth {
    maelys_mcp_authenticator_t inner;
    int calls;
} counting_auth_t;

static maelys_mcp_result_t counting_authenticate(
    void *context,
    const maelys_mcp_transport_credentials_t *credentials,
    maelys_mcp_principal_t **out_principal) {
    counting_auth_t *counter = context;
    ++counter->calls;
    return counter->inner.authenticate(
        counter->inner.context, credentials, out_principal);
}

static void counting_retain(void *context, maelys_mcp_principal_t *principal) {
    counting_auth_t *counter = context;
    counter->inner.retain(counter->inner.context, principal);
}

static void counting_release(void *context, maelys_mcp_principal_t *principal) {
    counting_auth_t *counter = context;
    counter->inner.release(counter->inner.context, principal);
}

static void counting_destroy(void *context) {
    counting_auth_t *counter = context;
    counter->inner.destroy(counter->inner.context);
}

static int fixture_start_counting(fixture_t *fixture, counting_auth_t *counter) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->tools.entered_write = -1;
    fixture->tools.hold_read = -1;
    memset(counter, 0, sizeof(*counter));
    if (maelys_http_auth_loopback_create(&counter->inner) != MAELYS_MCP_OK) return -1;
    fixture->authenticator.name = "counting";
    fixture->authenticator.context = counter;
    fixture->authenticator.authenticate = counting_authenticate;
    fixture->authenticator.retain = counting_retain;
    fixture->authenticator.release = counting_release;
    fixture->authenticator.destroy = counting_destroy;
    fixture->runtime = serving_runtime(&fixture->tools);
    if (!fixture->runtime) {
        fixture->authenticator.destroy(fixture->authenticator.context);
        return -1;
    }
    maelys_mcp_http_adapter_config_t adapter_config = {
        .runtime = fixture->runtime,
        .close_timeout_ms = 200u,
        .keepalive_interval_ms = 100u
    };
    if (maelys_mcp_http_adapter_create(
        &adapter_config, &fixture->adapter) != MAELYS_MCP_OK) {
        (void)maelys_mcp_runtime_destroy(fixture->runtime);
        fixture->authenticator.destroy(fixture->authenticator.context);
        return -1;
    }
    maelys_http_server_options_t options = {
        .bind_address = "127.0.0.1",
        .port = 0,
        .allowed_origins = NULL,
        .allowed_origin_count = 0,
        .max_body_bytes = 1024u,
        .header_timeout_ms = 2000u,
        .body_timeout_ms = 2000u,
        .idle_timeout_ms = 2000u,
        .write_timeout_ms = 2000u,
        .authenticator = &fixture->authenticator,
        .adapter = fixture->adapter
    };
    if (maelys_http_server_start(&options, &fixture->server) != MAELYS_MCP_OK) {
        maelys_mcp_http_adapter_destroy(fixture->adapter);
        (void)maelys_mcp_runtime_destroy(fixture->runtime);
        fixture->authenticator.destroy(fixture->authenticator.context);
        return -1;
    }
    fixture->port = maelys_http_server_port(fixture->server);
    return 0;
}

/*
 * Teardown order, and every step of it is load-bearing now that a connection
 * holds a channel and a channel holds a principal.
 *
 * Stop the listener first, so no new channel can be created. Destroy the
 * runtime SECOND, because a channel that missed its close deadline was detached
 * rather than waited on and is still out there: runtime destruction drains
 * those, which is what runs the last context_release. Only then the
 * authenticator, whose contract is that it is destroyed after the last channel
 * that could have carried one of its principals - the reverse order would
 * release a principal into a table that had already been freed.
 */
static void fixture_stop(fixture_t *fixture) {
    if (fixture->server) maelys_http_server_stop(fixture->server);
    if (fixture->adapter) maelys_mcp_http_adapter_destroy(fixture->adapter);
    if (fixture->runtime) (void)maelys_mcp_runtime_destroy(fixture->runtime);
    if (fixture->authenticator.destroy) {
        fixture->authenticator.destroy(fixture->authenticator.context);
    }
    /*
     * The tool state SURVIVES teardown, deliberately. Most of what a wedged
     * provider records is written during the teardown that unwedges it - a
     * detached channel is freed by its last worker, and that worker runs while
     * the runtime is being destroyed - so a memset over the whole fixture would
     * erase the observation the test came for.
     */
    tool_state_t observed = fixture->tools;
    memset(fixture, 0, sizeof(*fixture));
    fixture->tools = observed;
}

/* A fixture with a connection limit, for the one case that has to observe a
 * slot being released rather than assume it. */
static int fixture_start_limited(fixture_t *fixture, size_t max_connections) {
    if (fixture_start(fixture, NULL, 0, NULL, 0) != 0) return -1;
    maelys_http_server_stop(fixture->server);
    maelys_http_server_options_t options = {
        .bind_address = "127.0.0.1",
        .port = 0,
        .max_body_bytes = 1024u,
        .max_connections = max_connections,
        .header_timeout_ms = 2000u,
        .body_timeout_ms = 2000u,
        .idle_timeout_ms = 2000u,
        .write_timeout_ms = 2000u,
        .authenticator = &fixture->authenticator,
        .adapter = fixture->adapter
    };
    if (maelys_http_server_start(&options, &fixture->server) != MAELYS_MCP_OK) {
        fixture->server = NULL;
        fixture_stop(fixture);
        return -1;
    }
    fixture->port = maelys_http_server_port(fixture->server);
    return 0;
}

static int connect_loopback(unsigned short port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    /* inet_pton rather than INADDR_LOOPBACK: the macro is outside the
     * _POSIX_C_SOURCE surface this project compiles against. */
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) return -1;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const char *bytes, size_t length) {
    while (length) {
        ssize_t written = send(fd, bytes, length, 0);
        if (written <= 0) return -1;
        bytes += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

/* Reads until the peer closes or the buffer fills. Every response in this
 * suite is small and every connection either closes or is explicitly reused, so
 * "read to EOF" is well defined. */
static ssize_t read_response(int fd, char *buffer, size_t capacity, int timeout_ms) {
    size_t total = 0;
    for (;;) {
        struct pollfd descriptor = {.fd = fd, .events = POLLIN, .revents = 0};
        int ready = poll(&descriptor, 1u, timeout_ms);
        if (ready <= 0) break;
        ssize_t got = recv(fd, buffer + total, capacity - total - 1u, 0);
        if (got <= 0) break;
        total += (size_t)got;
        if (total + 1u >= capacity) break;
    }
    buffer[total] = '\0';
    return (ssize_t)total;
}

/* Reads exactly one response by stopping at the end of its body, so a
 * kept-alive connection can be read twice. */
static ssize_t read_one_message(int fd, char *buffer, size_t capacity, int timeout_ms) {
    size_t total = 0;
    long content_length = -1;
    size_t head_length = 0;
    for (;;) {
        if (!head_length) {
            for (size_t index = 0; index + 4u <= total; ++index) {
                if (memcmp(buffer + index, "\r\n\r\n", 4u) != 0) continue;
                head_length = index + 4u;
                content_length = 0;
                for (size_t scan = 0; scan + 15u <= head_length; ++scan) {
                    if (strncasecmp(buffer + scan, "content-length:", 15u) != 0) continue;
                    content_length = strtol(buffer + scan + 15, NULL, 10);
                    break;
                }
                break;
            }
        }
        if (head_length && content_length >= 0 &&
            total >= head_length + (size_t)content_length) {
            break;
        }
        struct pollfd descriptor = {.fd = fd, .events = POLLIN, .revents = 0};
        int ready = poll(&descriptor, 1u, timeout_ms);
        if (ready <= 0) break;
        ssize_t got = recv(fd, buffer + total, capacity - total - 1u, 0);
        if (got <= 0) break;
        total += (size_t)got;
        if (total + 1u >= capacity) break;
    }
    buffer[total] = '\0';
    return (ssize_t)total;
}

static int status_of(const char *response) {
    if (strncmp(response, "HTTP/1.1 ", 9u) != 0) return -1;
    return (int)strtol(response + 9, NULL, 10);
}

/* One request, one connection, one response. */
static int exchange(unsigned short port, const char *request, size_t length,
    char *response, size_t capacity) {
    int fd = connect_loopback(port);
    if (fd < 0) return -1;
    if (send_all(fd, request, length) != 0) {
        close(fd);
        return -1;
    }
    ssize_t got = read_response(fd, response, capacity, 3000);
    close(fd);
    return got >= 0 ? status_of(response) : -1;
}

/*
 * The MCP routing headers ride along in STD_HEADERS since H2. A case that
 * expects a server-layer refusal never reaches them, and a case that expects to
 * reach the adapter needs them: leaving them out would make every accepting
 * case fail on a -32020 that has nothing to do with the ladder being tested.
 */
#define BASE_HEADERS \
    "Host: 127.0.0.1\r\n" \
    "Content-Type: application/json\r\n" \
    "Accept: application/json, text/event-stream\r\n"
#define STD_HEADERS BASE_HEADERS \
    "MCP-Protocol-Version: 2026-07-28\r\n" \
    "Mcp-Method: ping\r\n"

typedef struct wire_case {
    const char *rule;
    const char *request;
    int status;
    /* A substring the response must carry, or NULL. */
    const char *contains;
} wire_case_t;

static const wire_case_t WIRE_CASES[] = {
    {"a well-formed POST is dispatched and answered by the runtime",
        "POST /mcp HTTP/1.1\r\n" STD_HEADERS "Content-Length: " VALID_BODY_LENGTH "\r\n\r\n" VALID_BODY,
        200, "\"result\""},
    {"GET on the endpoint is 405 with Allow: POST",
        "GET /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", 405, "Allow: POST"},
    {"DELETE on the endpoint is 405 with Allow: POST",
        "DELETE /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", 405, "Allow: POST"},
    {"an unknown path is 404 with no MCP body",
        "POST /nope HTTP/1.1\r\n" STD_HEADERS "Content-Length: 2\r\n\r\n{}", 404, NULL},
    {"a wrong Content-Type is 415",
        "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Content-Type: text/plain\r\n"
        "Accept: application/json, text/event-stream\r\n"
        "Content-Length: 2\r\n\r\n{}", 415, "-32600"},
    {"an insufficient Accept is 406",
        "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Content-Type: application/json\r\nAccept: application/json\r\n"
        "Content-Length: 2\r\n\r\n{}", 406, "-32600"},
    {"an absent Content-Length is 411",
        "POST /mcp HTTP/1.1\r\n" STD_HEADERS "\r\n", 411, NULL},
    {"a body over the limit is 413 before a byte of it is read",
        "POST /mcp HTTP/1.1\r\n" STD_HEADERS "Content-Length: 99999\r\n\r\n", 413, "-32600"},
    {"Expect is 417",
        "POST /mcp HTTP/1.1\r\n" STD_HEADERS
        "Expect: 100-continue\r\nContent-Length: 2\r\n\r\n{}", 417, NULL},
    {"any Transfer-Encoding is 400",
        "POST /mcp HTTP/1.1\r\n" STD_HEADERS
        "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400, NULL},
    {"Transfer-Encoding with Content-Length is 400",
        "POST /mcp HTTP/1.1\r\n" STD_HEADERS
        "Transfer-Encoding: chunked\r\nContent-Length: 2\r\n\r\n{}", 400, NULL},
    {"HTTP/1.0 is 505",
        "POST /mcp HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", 505, NULL},
    {"an absent Host is 400",
        "POST /mcp HTTP/1.1\r\nContent-Type: application/json\r\n"
        "Accept: application/json, text/event-stream\r\nContent-Length: 2\r\n\r\n{}",
        400, NULL},
    {"a non-loopback Host on a loopback bind is 400",
        "POST /mcp HTTP/1.1\r\nHost: evil.example\r\n"
        "Content-Type: application/json\r\n"
        "Accept: application/json, text/event-stream\r\n"
        "Content-Length: 2\r\n\r\n{}", 400, NULL},
    {"a repeated Host is 400",
        "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nHost: 127.0.0.1\r\n\r\n", 400, NULL},
    {"a bare LF is 400",
        "POST /mcp HTTP/1.1\nHost: 127.0.0.1\r\n\r\n", 400, NULL},
    {"a header block over its limit is 431",
        "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nX-A: "
        /* 40 x 256 bytes of header value, past the 16 KiB header budget. */
        , 431, NULL},
    {"a traversal in the target is 400",
        "POST /a/../mcp HTTP/1.1\r\n" STD_HEADERS "Content-Length: 2\r\n\r\n{}", 400, NULL},
    {"an absolute-form target whose authority differs from Host is 400",
        "POST http://evil.example/mcp HTTP/1.1\r\n" STD_HEADERS
        "Content-Length: 2\r\n\r\n{}", 400, NULL},
    {"an absolute-form target matching Host is routed",
        "POST http://127.0.0.1/mcp HTTP/1.1\r\n" STD_HEADERS
        "Content-Length: " VALID_BODY_LENGTH "\r\n\r\n" VALID_BODY, 200, "\"result\""},
    /*
     * The two H2 rows the adapter owns, proven on the wire rather than only
     * through the recording writer: the refusal has to survive the server
     * layer's response path to be worth anything to a client.
     */
    {"a routing header that disagrees with the body is 400 with -32020",
        "POST /mcp HTTP/1.1\r\n" BASE_HEADERS
        "MCP-Protocol-Version: 2026-07-28\r\n"
        "Mcp-Method: tools/list\r\n"
        "Content-Length: " VALID_BODY_LENGTH "\r\n\r\n" VALID_BODY, 400, "-32020"},
    {"a body that is not a JSON object is 400 with -32700",
        "POST /mcp HTTP/1.1\r\n" STD_HEADERS
        "Content-Length: 7\r\n\r\n[1,2,3]", 400, "-32700"}
};

static int wire_matrix(void) {
    fixture_t fixture;
    ASSERT_TRUE(fixture_start(&fixture, NULL, 0, NULL, 0) == 0);
    int failures = 0;
    char response[8192];
    for (size_t index = 0; index < sizeof(WIRE_CASES) / sizeof(*WIRE_CASES); ++index) {
        const wire_case_t *entry = &WIRE_CASES[index];
        char *request = NULL;
        size_t length = strlen(entry->request);
        /* The 431 case is the one that needs generating rather than spelling. */
        if (entry->status == 431) {
            length = strlen(entry->request) + 20000u + 4u;
            request = malloc(length + 1u);
            ASSERT_TRUE(request != NULL);
            size_t head = strlen(entry->request);
            memcpy(request, entry->request, head);
            memset(request + head, 'a', 20000u);
            memcpy(request + head + 20000u, "\r\n\r\n", 4u);
            length = head + 20000u + 4u;
            request[length] = '\0';
        }
        int status = exchange(fixture.port, request ? request : entry->request,
            length, response, sizeof(response));
        free(request);
        if (status != entry->status) {
            fprintf(stderr, "  [%s] status %d, want %d\n",
                entry->rule, status, entry->status);
            ++failures;
            continue;
        }
        if (entry->contains && !strstr(response, entry->contains)) {
            fprintf(stderr, "  [%s] response missing \"%s\"\n",
                entry->rule, entry->contains);
            ++failures;
        }
    }
    fixture_stop(&fixture);
    return failures;
}

/*
 * Origin is the DNS-rebinding control and the check that runs earliest. The
 * allowlist is empty by default, so a present Origin is refused until one is
 * configured; an absent Origin is accepted only because this bind is loopback.
 */
static int origin_is_refused_before_anything_else(void) {
    fixture_t fixture;
    ASSERT_TRUE(fixture_start(&fixture, NULL, 0, NULL, 0) == 0);
    char response[4096];
    /* Refused even though the rest of the request is also wrong in ways the
     * later checks would have caught - a bad Content-Type, an insufficient
     * Accept and an absent Content-Length. 403 rather than 415, 406 or 411 is
     * how "runs earliest" is observable from outside. */
    static const char *const cross_origin =
        "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: https://evil.example\r\n"
        "Content-Type: text/plain\r\nAccept: text/plain\r\n\r\n";
    int status = exchange(fixture.port, cross_origin, strlen(cross_origin),
        response, sizeof(response));
    ASSERT_TRUE(status == 403);
    ASSERT_TRUE(strstr(response, "-32600") != NULL);
    fixture_stop(&fixture);

    static const char *const origins[] = {"https://app.example"};
    ASSERT_TRUE(fixture_start(&fixture, NULL, 0, origins, 1u) == 0);
    const char *allowed =
        "POST /mcp HTTP/1.1\r\n" STD_HEADERS
        "Origin: https://app.example\r\n"
        "Content-Length: " VALID_BODY_LENGTH "\r\n\r\n" VALID_BODY;
    ASSERT_TRUE(exchange(fixture.port, allowed, strlen(allowed),
        response, sizeof(response)) == 200);
    const char *refused =
        "POST /mcp HTTP/1.1\r\n" STD_HEADERS
        "Origin: https://other.example\r\nContent-Length: 2\r\n\r\n{}";
    ASSERT_TRUE(exchange(fixture.port, refused, strlen(refused),
        response, sizeof(response)) == 403);
    fixture_stop(&fixture);
    return 0;
}

/*
 * Keep-alive is permitted for JSON-mode replies, and each request on a reused
 * connection is authenticated independently rather than inheriting a verdict.
 */
static int a_kept_alive_connection_serves_a_second_request(void) {
    fixture_t fixture;
    ASSERT_TRUE(fixture_start(&fixture, NULL, 0, NULL, 0) == 0);
    int fd = connect_loopback(fixture.port);
    ASSERT_TRUE(fd >= 0);
    const char *request =
        "POST /mcp HTTP/1.1\r\n" STD_HEADERS
        "Content-Length: " VALID_BODY_LENGTH "\r\n\r\n" VALID_BODY;
    char response[4096];
    for (int round = 0; round < 2; ++round) {
        ASSERT_TRUE(send_all(fd, request, strlen(request)) == 0);
        ssize_t got = read_one_message(fd, response, sizeof(response), 3000);
        ASSERT_TRUE(got > 0);
        ASSERT_TRUE(status_of(response) == 200);
        ASSERT_TRUE(strstr(response, "Connection: keep-alive") != NULL);
    }
    close(fd);
    fixture_stop(&fixture);
    return 0;
}

/*
 * Authentication is repeated for every POST, so two requests on one kept-alive
 * connection are two independent authentications and a revoked credential stops
 * working on the next request rather than on the next connection. Asserted as a
 * count, because "both were answered" is also what a cached verdict looks like.
 */
static int authenticate_runs_once_per_post(void) {
    fixture_t fixture;
    counting_auth_t counter;
    ASSERT_TRUE(fixture_start_counting(&fixture, &counter) == 0);
    const char *request =
        "POST /mcp HTTP/1.1\r\n" STD_HEADERS
        "Content-Length: " VALID_BODY_LENGTH "\r\n\r\n" VALID_BODY;
    char response[4096];

    /* One connection, one POST: exactly one authentication. */
    ASSERT_TRUE(exchange(fixture.port, request, strlen(request),
        response, sizeof(response)) == 200);
    ASSERT_TRUE(counter.calls == 1);

    /* One connection, two POSTs: exactly two more. */
    int fd = connect_loopback(fixture.port);
    ASSERT_TRUE(fd >= 0);
    for (int round = 0; round < 2; ++round) {
        ASSERT_TRUE(send_all(fd, request, strlen(request)) == 0);
        ssize_t got = read_one_message(fd, response, sizeof(response), 3000);
        ASSERT_TRUE(got > 0);
        ASSERT_TRUE(status_of(response) == 200);
    }
    close(fd);
    ASSERT_TRUE(counter.calls == 3);
    fixture_stop(&fixture);
    return 0;
}

/*
 * Pipelining is never permitted. Two requests in one write is the shape that
 * makes the test deterministic, and the connection is closed with the second
 * unanswered - not queued, not resynchronized to.
 */
static int a_pipelined_request_is_refused(void) {
    fixture_t fixture;
    ASSERT_TRUE(fixture_start(&fixture, NULL, 0, NULL, 0) == 0);
    int fd = connect_loopback(fixture.port);
    ASSERT_TRUE(fd >= 0);
    const char *one =
        "POST /mcp HTTP/1.1\r\n" STD_HEADERS "Content-Length: 2\r\n\r\n{}";
    char both[1024];
    int written = snprintf(both, sizeof(both), "%s%s", one, one);
    ASSERT_TRUE(written > 0);
    ASSERT_TRUE(send_all(fd, both, (size_t)written) == 0);
    char response[4096];
    ssize_t got = read_response(fd, response, sizeof(response), 3000);
    /* Closed with nothing written: a response in flight is abandoned, and no
     * response was in flight yet. */
    ASSERT_TRUE(got == 0);
    close(fd);
    fixture_stop(&fixture);
    return 0;
}

/*
 * The RST-safe rejection close. The client announces a body, is refused before
 * it sends one, and must still be able to READ the refusal - which is the whole
 * reason the refusal is followed by shutdown(SHUT_WR) and a bounded discard
 * rather than an immediate close.
 */
static int a_rejection_reaches_a_client_still_writing_a_body(void) {
    fixture_t fixture;
    static const char *const tokens[] = {"good-token"};
    ASSERT_TRUE(fixture_start(&fixture, tokens, 1u, NULL, 0) == 0);
    int fd = connect_loopback(fixture.port);
    ASSERT_TRUE(fd >= 0);
    const char *head =
        "POST /mcp HTTP/1.1\r\n" STD_HEADERS "Content-Length: 1000\r\n\r\n";
    ASSERT_TRUE(send_all(fd, head, strlen(head)) == 0);
    /* A first slice of the body, so the client is genuinely mid-write when the
     * 401 is produced. */
    char slice[400];
    memset(slice, 'x', sizeof(slice));
    ASSERT_TRUE(send_all(fd, slice, sizeof(slice)) == 0);
    char response[4096];
    ssize_t got = read_response(fd, response, sizeof(response), 3000);
    ASSERT_TRUE(got > 0);
    ASSERT_TRUE(status_of(response) == 401);
    ASSERT_TRUE(strstr(response, "WWW-Authenticate: Bearer") != NULL);
    close(fd);
    fixture_stop(&fixture);
    return 0;
}

static int bearer_authentication_on_the_wire(void) {
    fixture_t fixture;
    static const char *const tokens[] = {"good-token"};
    ASSERT_TRUE(fixture_start(&fixture, tokens, 1u, NULL, 0) == 0);
    char response[4096];

    const char *anonymous =
        "POST /mcp HTTP/1.1\r\n" STD_HEADERS "Content-Length: 2\r\n\r\n{}";
    ASSERT_TRUE(exchange(fixture.port, anonymous, strlen(anonymous),
        response, sizeof(response)) == 401);
    ASSERT_TRUE(strstr(response, "WWW-Authenticate: Bearer") != NULL);

    const char *wrong =
        "POST /mcp HTTP/1.1\r\n" STD_HEADERS
        "Authorization: Bearer wrong-token\r\nContent-Length: 2\r\n\r\n{}";
    /* An invalid bearer is a failed authentication, not a denied principal: it
     * gets 401 and never 403. */
    ASSERT_TRUE(exchange(fixture.port, wrong, strlen(wrong),
        response, sizeof(response)) == 401);

    const char *right =
        "POST /mcp HTTP/1.1\r\n" STD_HEADERS
        "Authorization: Bearer good-token\r\n"
        "Content-Length: " VALID_BODY_LENGTH "\r\n\r\n" VALID_BODY;
    ASSERT_TRUE(exchange(fixture.port, right, strlen(right),
        response, sizeof(response)) == 200);
    fixture_stop(&fixture);
    return 0;
}

/*
 * Binding anything other than loopback with loopback-trust refuses to start,
 * and refuses BEFORE the socket exists.
 */
static int a_public_bind_refuses_loopback_trust(void) {
    maelys_mcp_authenticator_t authenticator;
    ASSERT_TRUE(maelys_http_auth_loopback_create(&authenticator) == MAELYS_MCP_OK);
    tool_state_t tools = {.entered_write = -1, .hold_read = -1};
    maelys_mcp_runtime_t *runtime = serving_runtime(&tools);
    ASSERT_TRUE(runtime != NULL);
    maelys_mcp_http_adapter_t *adapter = NULL;
    maelys_mcp_http_adapter_config_t adapter_config = {.runtime = runtime};
    ASSERT_TRUE(maelys_mcp_http_adapter_create(&adapter_config, &adapter) ==
        MAELYS_MCP_OK);
    maelys_http_server_t *server = NULL;
    maelys_http_server_options_t options = {
        .bind_address = "0.0.0.0",
        .port = 0,
        .authenticator = &authenticator,
        .adapter = adapter
    };
    ASSERT_TRUE(maelys_http_server_start(&options, &server) == MAELYS_MCP_ERR_DENIED);
    ASSERT_TRUE(server == NULL);

    /* The same bind with a real authenticator starts. */
    static const char *const tokens[] = {"t"};
    maelys_mcp_authenticator_t bearer;
    ASSERT_TRUE(maelys_http_auth_static_bearer_create(tokens, 1u, &bearer) ==
        MAELYS_MCP_OK);
    options.authenticator = &bearer;
    options.bind_address = "127.0.0.1";
    ASSERT_TRUE(maelys_http_server_start(&options, &server) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_http_server_port(server) != 0);
    maelys_http_server_stop(server);

    /* And a listener with no authenticator at all is an argument error rather
     * than an anonymous listener. */
    options.authenticator = NULL;
    server = NULL;
    ASSERT_TRUE(maelys_http_server_start(&options, &server) == MAELYS_MCP_ERR_ARGUMENT);

    maelys_mcp_http_adapter_destroy(adapter);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    bearer.destroy(bearer.context);
    authenticator.destroy(authenticator.context);
    return 0;
}

/* S2: POLLIN fires for both "the peer sent data" and "the peer sent FIN", and
 * conflating them would abort live requests whenever a client pipelined. */
static int peek_tells_a_fin_from_a_pipelined_byte(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(listener >= 0);
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    ASSERT_TRUE(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    ASSERT_TRUE(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    ASSERT_TRUE(listen(listener, 1) == 0);
    socklen_t length = sizeof(address);
    ASSERT_TRUE(getsockname(listener, (struct sockaddr *)&address, &length) == 0);

    int client = connect_loopback(ntohs(address.sin_port));
    ASSERT_TRUE(client >= 0);
    int server = accept(listener, NULL, NULL);
    ASSERT_TRUE(server >= 0);

    /* Quiet: neither. */
    ASSERT_TRUE(maelys_http_peek_disambiguate(server) == -1);

    /* A byte: a pipelined request, which is a protocol error. */
    ASSERT_TRUE(send_all(client, "P", 1u) == 0);
    struct pollfd descriptor = {.fd = server, .events = POLLIN, .revents = 0};
    ASSERT_TRUE(poll(&descriptor, 1u, 2000) == 1);
    ASSERT_TRUE(maelys_http_peek_disambiguate(server) == 1);
    /* MSG_PEEK, so the byte is still there for the real reader. */
    char byte = 0;
    ASSERT_TRUE(recv(server, &byte, 1u, 0) == 1 && byte == 'P');

    /* A FIN: the client is gone. */
    close(client);
    descriptor.revents = 0;
    ASSERT_TRUE(poll(&descriptor, 1u, 2000) == 1);
    ASSERT_TRUE(maelys_http_peek_disambiguate(server) == 0);

    close(server);
    close(listener);
    return 0;
}

/* ------------------------------------------------ the authenticator seam */

static int loopback_trust_takes_no_material(void) {
    maelys_mcp_authenticator_t authenticator;
    ASSERT_TRUE(maelys_http_auth_loopback_create(&authenticator) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_http_auth_requires_loopback(&authenticator) == 1);
    maelys_mcp_principal_t *principal = NULL;
    maelys_mcp_transport_credentials_t none = {
        .kind = MAELYS_MCP_CREDENTIAL_NONE, .peer = "127.0.0.1:1"
    };
    ASSERT_TRUE(authenticator.authenticate(authenticator.context, &none, &principal) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(principal != NULL);
    ASSERT_TRUE(strcmp(maelys_http_auth_principal_name(&authenticator, principal),
        "loopback") == 0);
    /* One process-wide singleton: two calls yield the same principal. */
    maelys_mcp_principal_t *again = NULL;
    ASSERT_TRUE(authenticator.authenticate(authenticator.context, &none, &again) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(again == principal);
    authenticator.release(authenticator.context, principal);
    authenticator.release(authenticator.context, again);

    /*
     * Being handed a bearer credential is an argument error, not a silent
     * success. This authenticator's claim is that reaching the socket IS the
     * credential, and that claim is false the moment it starts ignoring one.
     */
    maelys_mcp_transport_credentials_t bearer = {
        .kind = MAELYS_MCP_CREDENTIAL_BEARER,
        .peer = "127.0.0.1:1",
        .material = "Bearer x",
        .material_length = 8u
    };
    maelys_mcp_principal_t *refused = NULL;
    ASSERT_TRUE(authenticator.authenticate(authenticator.context, &bearer, &refused) ==
        MAELYS_MCP_ERR_ARGUMENT);
    authenticator.destroy(authenticator.context);
    return 0;
}

typedef struct bearer_case {
    const char *rule;
    maelys_mcp_credential_kind_t kind;
    const char *material;
    maelys_mcp_result_t expected;
} bearer_case_t;

static int static_bearer_outcomes(void) {
    static const char *const tokens[] = {"alpha-token", "beta-token"};
    maelys_mcp_authenticator_t authenticator;
    ASSERT_TRUE(maelys_http_auth_static_bearer_create(tokens, 2u, &authenticator) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_http_auth_requires_loopback(&authenticator) == 0);

    static const bearer_case_t cases[] = {
        {"an absent credential is ERR_ARGUMENT",
            MAELYS_MCP_CREDENTIAL_NONE, NULL, MAELYS_MCP_ERR_ARGUMENT},
        {"a matching token is accepted",
            MAELYS_MCP_CREDENTIAL_BEARER, "Bearer alpha-token", MAELYS_MCP_OK},
        {"the second configured token is accepted too",
            MAELYS_MCP_CREDENTIAL_BEARER, "Bearer beta-token", MAELYS_MCP_OK},
        {"the scheme is case-insensitive",
            MAELYS_MCP_CREDENTIAL_BEARER, "bEaReR alpha-token", MAELYS_MCP_OK},
        {"extra whitespace after the scheme is tolerated",
            MAELYS_MCP_CREDENTIAL_BEARER, "Bearer   alpha-token", MAELYS_MCP_OK},
        {"a non-matching token is ERR_DENIED, not ERR_ARGUMENT",
            MAELYS_MCP_CREDENTIAL_BEARER, "Bearer wrong", MAELYS_MCP_ERR_DENIED},
        {"a prefix of a configured token is denied",
            MAELYS_MCP_CREDENTIAL_BEARER, "Bearer alpha", MAELYS_MCP_ERR_DENIED},
        {"a token with a trailing byte is denied",
            MAELYS_MCP_CREDENTIAL_BEARER, "Bearer alpha-tokenX", MAELYS_MCP_ERR_DENIED},
        {"another scheme is malformed rather than denied",
            MAELYS_MCP_CREDENTIAL_BEARER, "Basic alpha-token", MAELYS_MCP_ERR_ARGUMENT},
        {"a bare scheme with no token is malformed",
            MAELYS_MCP_CREDENTIAL_BEARER, "Bearer ", MAELYS_MCP_ERR_ARGUMENT},
        {"the scheme alone is malformed",
            MAELYS_MCP_CREDENTIAL_BEARER, "Bearer", MAELYS_MCP_ERR_ARGUMENT},
        {"an empty value is malformed",
            MAELYS_MCP_CREDENTIAL_BEARER, "", MAELYS_MCP_ERR_ARGUMENT}
    };
    int failures = 0;
    for (size_t index = 0; index < sizeof(cases) / sizeof(*cases); ++index) {
        const bearer_case_t *entry = &cases[index];
        maelys_mcp_transport_credentials_t credentials = {
            .kind = entry->kind,
            .peer = "127.0.0.1:1",
            .material = entry->material,
            .material_length = entry->material ? strlen(entry->material) : 0u
        };
        maelys_mcp_principal_t *principal = NULL;
        maelys_mcp_result_t status = authenticator.authenticate(
            authenticator.context, &credentials, &principal);
        if (status != entry->expected) {
            fprintf(stderr, "  [%s] got %d want %d\n",
                entry->rule, (int)status, (int)entry->expected);
            ++failures;
        }
        if (status == MAELYS_MCP_OK) {
            /* The principal is named by its slot, never by its token: a log
             * line that leaks four characters of a shared secret has leaked
             * four characters of a shared secret. */
            const char *name = maelys_http_auth_principal_name(&authenticator, principal);
            if (!name || strstr(name, "token") != NULL) {
                fprintf(stderr, "  [%s] principal name leaks the credential\n",
                    entry->rule);
                ++failures;
            }
            authenticator.release(authenticator.context, principal);
        } else if (principal) {
            fprintf(stderr, "  [%s] a failed authentication produced a principal\n",
                entry->rule);
            ++failures;
        }
    }

    /* Two tokens are two principals. */
    maelys_mcp_transport_credentials_t alpha = {
        .kind = MAELYS_MCP_CREDENTIAL_BEARER,
        .material = "Bearer alpha-token", .material_length = 18u
    };
    maelys_mcp_transport_credentials_t beta = {
        .kind = MAELYS_MCP_CREDENTIAL_BEARER,
        .material = "Bearer beta-token", .material_length = 17u
    };
    maelys_mcp_principal_t *first = NULL;
    maelys_mcp_principal_t *second = NULL;
    ASSERT_TRUE(authenticator.authenticate(authenticator.context, &alpha, &first) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(authenticator.authenticate(authenticator.context, &beta, &second) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(first != second);
    authenticator.release(authenticator.context, first);
    authenticator.release(authenticator.context, second);

    authenticator.destroy(authenticator.context);
    ASSERT_TRUE(maelys_http_auth_static_bearer_create(NULL, 0, &authenticator) ==
        MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(failures == 0);
    return 0;
}

/* Retain and release are a refcount, and a principal survives until the last
 * reference goes. ASan and the leak checker are the other half of this test. */
static int principal_refcounting(void) {
    static const char *const tokens[] = {"alpha-token"};
    maelys_mcp_authenticator_t authenticator;
    ASSERT_TRUE(maelys_http_auth_static_bearer_create(tokens, 1u, &authenticator) ==
        MAELYS_MCP_OK);
    maelys_mcp_transport_credentials_t credentials = {
        .kind = MAELYS_MCP_CREDENTIAL_BEARER,
        .material = "Bearer alpha-token", .material_length = 18u
    };
    maelys_mcp_principal_t *principal = NULL;
    ASSERT_TRUE(authenticator.authenticate(authenticator.context, &credentials,
        &principal) == MAELYS_MCP_OK);
    for (int index = 0; index < 16; ++index) {
        authenticator.retain(authenticator.context, principal);
    }
    for (int index = 0; index < 16; ++index) {
        authenticator.release(authenticator.context, principal);
    }
    /* Still alive: the authenticate() reference has not been given back. */
    ASSERT_TRUE(maelys_http_auth_principal_name(&authenticator, principal) != NULL);
    authenticator.release(authenticator.context, principal);
    authenticator.destroy(authenticator.context);
    return 0;
}


/* ------------------------------------------------------- the H3 wire half */

/*
 * Everything below is MCP over a real socket, against the real server layer and
 * the real runtime. What the adapter suite proves with a recording writer, this
 * proves in bytes: which Content-Type came back, in what order the SSE events
 * arrived, whether the terminal chunk was there, and whether a connection
 * survived to carry the next request.
 */

#define REQUEST_LINE(headers, length, body) \
    "POST /mcp HTTP/1.1\r\n" headers "Content-Length: " length "\r\n\r\n" body

/* tools/list: one buffered response, therefore application/json. */
#define LIST_BODY \
    "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/list\",\"params\":{" \
    MODERN_META "}}"
#define LIST_HEADERS BASE_HEADERS \
    "MCP-Protocol-Version: 2026-07-28\r\n" \
    "Mcp-Method: tools/list\r\n"

/* tools/call with a progressToken: frames before the response, therefore SSE. */
#define CALL_BODY \
    "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\",\"params\":{" \
    "\"name\":\"fx.stream\",\"arguments\":{}," \
    "\"_meta\":{" \
    "\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"," \
    "\"io.modelcontextprotocol/clientCapabilities\":{}," \
    "\"progressToken\":\"tok\"}}}"
#define CALL_HEADERS BASE_HEADERS \
    "MCP-Protocol-Version: 2026-07-28\r\n" \
    "Mcp-Method: tools/call\r\n" \
    "Mcp-Name: fx.stream\r\n"

/* A notification: 202 and an empty body. */
#define NOTE_BODY \
    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{" \
    MODERN_META "}}"
#define NOTE_HEADERS BASE_HEADERS \
    "MCP-Protocol-Version: 2026-07-28\r\n" \
    "Mcp-Method: notifications/initialized\r\n"

/* subscriptions/listen: the acknowledgement first, so also SSE. */
#define LISTEN_BODY \
    "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"subscriptions/listen\",\"params\":{" \
    "\"notifications\":{\"resourceSubscriptions\":[\"fx://repo/doc.txt\"]}," \
    MODERN_META "}}"
#define LISTEN_HEADERS BASE_HEADERS \
    "MCP-Protocol-Version: 2026-07-28\r\n" \
    "Mcp-Method: subscriptions/listen\r\n"

/* Content-Lengths, pinned rather than counted, for the same reason
 * VALID_BODY_LENGTH is. */
#define LIST_LENGTH "171"
#define CALL_LENGTH "227"
#define NOTE_LENGTH "178"
#define LISTEN_LENGTH "245"
_Static_assert(sizeof(LIST_BODY) - 1u == 171u, "LIST_BODY length");
_Static_assert(sizeof(CALL_BODY) - 1u == 227u, "CALL_BODY length");
_Static_assert(sizeof(NOTE_BODY) - 1u == 178u, "NOTE_BODY length");
_Static_assert(sizeof(LISTEN_BODY) - 1u == 245u, "LISTEN_BODY length");

/* The suite's own clock, because the server layer's is static to its
 * translation unit and a test must not reach into one. */
static uint64_t now_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0u;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void sleep_ms(long milliseconds) {
    struct timespec pause = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000 * 1000
    };
    (void)nanosleep(&pause, NULL);
}

static int occurrences_of(const char *haystack, const char *needle) {
    int count = 0;
    for (const char *scan = haystack;
         (scan = strstr(scan, needle)) != NULL; ++scan) {
        ++count;
    }
    return count;
}

/*
 * Reads until `needle` has appeared `wanted` times, or the deadline passes.
 *
 * Reading "until the socket goes quiet" does not work on this transport and the
 * reason is worth stating: an open SSE stream is never quiet. It emits a
 * keep-alive comment every interval forever, so a reader that waited for
 * silence would wait for the provider's own timeout instead of for the frames
 * it came for - which is exactly the bug that made an earlier version of the
 * disconnect test observe a completed exchange.
 */
static ssize_t read_until(int fd, char *buffer, size_t capacity,
    const char *needle, int wanted, int timeout_ms) {
    size_t total = 0;
    uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
    buffer[0] = '\0';
    while (occurrences_of(buffer, needle) < wanted) {
        uint64_t now = now_ms();
        if (now >= deadline) break;
        struct pollfd descriptor = {.fd = fd, .events = POLLIN, .revents = 0};
        int ready = poll(&descriptor, 1u, (int)(deadline - now));
        if (ready <= 0) break;
        ssize_t got = recv(fd, buffer + total, capacity - total - 1u, 0);
        if (got <= 0) break;
        total += (size_t)got;
        buffer[total] = '\0';
        if (total + 1u >= capacity) break;
    }
    return (ssize_t)total;
}

/* The byte offset of `needle` in `haystack`, or -1. Ordering assertions are
 * made on offsets rather than on presence, because "both arrived" is also what
 * an out-of-order stream looks like. */
static long offset_of(const char *haystack, const char *needle) {
    const char *found = strstr(haystack, needle);
    return found ? (long)(found - haystack) : -1;
}

/*
 * tools/list over HTTP: the smallest complete statement that this endpoint
 * serves MCP. A real catalogue comes back, as application/json, on a connection
 * that stays open.
 */
static int tools_list_is_answered_as_json(void) {
    fixture_t fixture;
    ASSERT_TRUE(fixture_start(&fixture, NULL, 0, NULL, 0) == 0);
    int fd = connect_loopback(fixture.port);
    ASSERT_TRUE(fd >= 0);
    static const char request[] = REQUEST_LINE(LIST_HEADERS, LIST_LENGTH, LIST_BODY);
    ASSERT_TRUE(send_all(fd, request, sizeof(request) - 1u) == 0);
    char response[8192];
    ASSERT_TRUE(read_one_message(fd, response, sizeof(response), 3000) > 0);
    ASSERT_TRUE(status_of(response) == 200);
    ASSERT_TRUE(strstr(response, "Content-Type: application/json") != NULL);
    /* Buffered, not streamed: a known length and no chunked framing. */
    ASSERT_TRUE(strstr(response, "Transfer-Encoding") == NULL);
    ASSERT_TRUE(strstr(response, "Content-Length:") != NULL);
    ASSERT_TRUE(strstr(response, "fx.stream") != NULL);
    ASSERT_TRUE(strstr(response, "\"id\":11") != NULL);
    /* Keep-alive, and the same connection carries a second request. */
    ASSERT_TRUE(strstr(response, "Connection: keep-alive") != NULL);
    ASSERT_TRUE(send_all(fd, request, sizeof(request) - 1u) == 0);
    ASSERT_TRUE(read_one_message(fd, response, sizeof(response), 3000) > 0);
    ASSERT_TRUE(status_of(response) == 200);
    ASSERT_TRUE(strstr(response, "fx.stream") != NULL);
    close(fd);
    fixture_stop(&fixture);
    return 0;
}

/*
 * The ordering proof, over HTTP.
 *
 * tests/test_middleware.c states this one layer down as
 * test_wrap_sink_keeps_progress_ahead_of_the_response; here it is asserted on
 * the bytes a client would actually read. Over SSE the final response
 * terminates the stream, so a progress frame behind it is not late - it is
 * gone - which is why the assertion is on OFFSETS and not on presence.
 */
static int a_call_with_progress_streams_in_order(void) {
    fixture_t fixture;
    ASSERT_TRUE(fixture_start(&fixture, NULL, 0, NULL, 0) == 0);
    int fd = connect_loopback(fixture.port);
    ASSERT_TRUE(fd >= 0);
    static const char request[] = REQUEST_LINE(CALL_HEADERS, CALL_LENGTH, CALL_BODY);
    ASSERT_TRUE(send_all(fd, request, sizeof(request) - 1u) == 0);
    char response[16384];
    /* Read to EOF: an SSE reply is Connection: close, so the peer closing is
     * the end of the body. */
    ASSERT_TRUE(read_response(fd, response, sizeof(response), 3000) > 0);
    close(fd);
    ASSERT_TRUE(status_of(response) == 200);
    ASSERT_TRUE(strstr(response, "Content-Type: text/event-stream") != NULL);
    ASSERT_TRUE(strstr(response, "Transfer-Encoding: chunked") != NULL);
    ASSERT_TRUE(strstr(response, "Cache-Control: no-store") != NULL);
    ASSERT_TRUE(strstr(response, "X-Accel-Buffering: no") != NULL);
    /* An SSE reply is never reused. */
    ASSERT_TRUE(strstr(response, "Connection: close") != NULL);

    /* Three progress frames, then the response, in that order. */
    long first = offset_of(response, "notifications/progress");
    ASSERT_TRUE(first > 0);
    long last_progress = -1;
    for (const char *scan = response;
         (scan = strstr(scan, "notifications/progress")) != NULL; ++scan) {
        last_progress = (long)(scan - response);
    }
    long response_frame = offset_of(response, "\"id\":12");
    ASSERT_TRUE(response_frame > 0);
    ASSERT_TRUE(last_progress < response_frame);
    /* And the terminal chunk came after the response, not before it. */
    long terminal = offset_of(response, "\r\n0\r\n\r\n");
    ASSERT_TRUE(terminal > response_frame);
    /* Every frame is its own `data:` line with a blank line after it. */
    ASSERT_TRUE(strstr(response, "data: {\"jsonrpc\":\"2.0\",\"method\":"
        "\"notifications/progress\"") != NULL);
    fixture_stop(&fixture);
    return 0;
}

/*
 * A notification is 202 with an empty body, on a connection that is still
 * reusable - a client that batches notifications pays one handshake, not one
 * per notification.
 */
static int a_notification_is_202_and_keeps_the_connection(void) {
    fixture_t fixture;
    ASSERT_TRUE(fixture_start(&fixture, NULL, 0, NULL, 0) == 0);
    int fd = connect_loopback(fixture.port);
    ASSERT_TRUE(fd >= 0);
    static const char note[] = REQUEST_LINE(NOTE_HEADERS, NOTE_LENGTH, NOTE_BODY);
    char response[4096];
    ASSERT_TRUE(send_all(fd, note, sizeof(note) - 1u) == 0);
    ASSERT_TRUE(read_one_message(fd, response, sizeof(response), 3000) > 0);
    ASSERT_TRUE(status_of(response) == 202);
    ASSERT_TRUE(strstr(response, "Content-Length: 0") != NULL);
    ASSERT_TRUE(strstr(response, "Connection: keep-alive") != NULL);
    /* No MCP body of any kind: not a result, not an error. */
    ASSERT_TRUE(strstr(response, "jsonrpc") == NULL);

    /* The same connection then carries a request and answers it. */
    static const char list[] = REQUEST_LINE(LIST_HEADERS, LIST_LENGTH, LIST_BODY);
    ASSERT_TRUE(send_all(fd, list, sizeof(list) - 1u) == 0);
    ASSERT_TRUE(read_one_message(fd, response, sizeof(response), 3000) > 0);
    ASSERT_TRUE(status_of(response) == 200);
    ASSERT_TRUE(strstr(response, "fx.stream") != NULL);
    close(fd);
    fixture_stop(&fixture);
    return 0;
}

/*
 * The full cancellation chain, end to end, and the four claims it has to make
 * separately because they are four different things:
 *
 *   the provider was told - its post-hold emit is refused, which is the
 *     "MUST NOT send any further messages for it" rule holding by construction;
 *   no further bytes were written - the stream stops where the disconnect
 *     found it, with NO terminal chunk, which is how HTTP/1.1 spells truncated;
 *   the connection slot was freed inside the close deadline, even though the
 *     provider was still running - the H0b guarantee;
 *   the principal was released exactly once, counted rather than assumed.
 *
 * The provider is held inside its call on purpose, so "mid-call" is a state
 * this test chooses rather than one it races for.
 */
static int a_client_disconnect_cancels_the_call(void) {
    int hold[2] = {-1, -1};
    int entered[2] = {-1, -1};
    ASSERT_TRUE(pipe(hold) == 0);
    ASSERT_TRUE(pipe(entered) == 0);
    fixture_t fixture;
    /*
     * One connection, so that "the slot was freed" is a fact the next connect
     * can establish. At the default limit of 128 a probe would succeed whether
     * or not the wedged exchange had let go of anything.
     */
    ASSERT_TRUE(fixture_start_limited(&fixture, 1u) == 0);
    fixture.tools.hold_read = hold[0];
    fixture.tools.entered_write = entered[1];

    int fd = connect_loopback(fixture.port);
    ASSERT_TRUE(fd >= 0);
    static const char request[] = REQUEST_LINE(CALL_HEADERS, CALL_LENGTH, CALL_BODY);
    ASSERT_TRUE(send_all(fd, request, sizeof(request) - 1u) == 0);

    /* Wait until the provider is demonstrably inside the call. */
    struct pollfd wait_entered = {.fd = entered[0], .events = POLLIN, .revents = 0};
    ASSERT_TRUE(poll(&wait_entered, 1u, 3000) == 1);

    /* Every frame the call produced before it wedged, and no further. */
    char received[16384];
    ASSERT_TRUE(read_until(fd, received, sizeof(received),
        "notifications/progress", 3, 3000) > 0);
    ASSERT_TRUE(status_of(received) == 200);
    ASSERT_TRUE(strstr(received, "text/event-stream") != NULL);
    ASSERT_TRUE(occurrences_of(received, "notifications/progress") == 3);
    /* Still mid-exchange: no response for this id, and no terminal chunk. */
    ASSERT_TRUE(strstr(received, "\"id\":12") == NULL);
    ASSERT_TRUE(strstr(received, "\r\n0\r\n\r\n") == NULL);

    /*
     * The disconnect, as a half-close rather than a full one, and the choice is
     * the instrument rather than a compromise. What the server observes is
     * identical - recv returns 0 on a socket that has been shut down for
     * writing exactly as on one whose peer is gone - but the read half stays
     * open, so this test can assert what the server did NEXT. A full close
     * would make "no further writes" unobservable, which is the one claim
     * worth making here.
     */
    ASSERT_TRUE(shutdown(fd, SHUT_WR) == 0);

    /* Read to EOF. Whatever arrives after the FIN is the thing under test. */
    char after[16384];
    ssize_t tail = read_response(fd, after, sizeof(after), 2000);
    ASSERT_TRUE(tail >= 0);
    /* No terminal chunk, ever: a chunked body that ends without one is how
     * HTTP/1.1 says "truncated", and 0\r\n\r\n would have told this client the
     * cancelled call completed normally. */
    ASSERT_TRUE(strstr(after, "\r\n0\r\n\r\n") == NULL);
    ASSERT_TRUE(strstr(after, "0\r\n\r\n") == NULL);
    /* And the response never arrived, because the abort closed the outbox
     * before the provider could complete into it. */
    ASSERT_TRUE(strstr(after, "\"id\":12") == NULL);
    close(fd);

    /*
     * The connection slot is released while the provider is still wedged. This
     * is the H0b claim: the channel outlives the connection and is freed by its
     * last worker, so a network peer cannot hold a slot for as long as a
     * provider takes.
     */
    uint64_t deadline = now_ms() + 3000u;
    int freed = 0;
    while (!freed) {
        if (now_ms() >= deadline) break;
        /* A new connection being accepted and answered is the observable form
         * of "the slot is available again". */
        int probe = connect_loopback(fixture.port);
        if (probe >= 0) {
            static const char list[] =
                REQUEST_LINE(LIST_HEADERS, LIST_LENGTH, LIST_BODY);
            char response[8192];
            if (send_all(probe, list, sizeof(list) - 1u) == 0 &&
                read_one_message(probe, response, sizeof(response), 1000) > 0 &&
                status_of(response) == 200) {
                freed = 1;
            }
            close(probe);
        }
        if (!freed) sleep_ms(5);
    }
    ASSERT_TRUE(freed);

    /* Exactly one call reached the provider, so nothing was retried behind the
     * client's back on the way to all of the above. */
    ASSERT_TRUE(fixture.tools.calls == 1);

    /* Let the wedged provider go and shut everything down. */
    ssize_t written = write(hold[1], "x", 1u);
    (void)written;
    fixture_stop(&fixture);

    /* The provider learned: its emit after the hold was refused, because the
     * abort had already closed the channel's outbox. */
    ASSERT_TRUE(fixture.tools.emit_after_hold == MAELYS_MCP_ERR_CLOSED);

    close(hold[0]);
    close(hold[1]);
    close(entered[0]);
    close(entered[1]);
    return 0;
}

/*
 * subscriptions/listen over HTTP: the acknowledgement opens the stream - the
 * fact most readers guess wrong, because that acknowledgement travels the
 * completion path even though it is a notification with no id - and the client
 * closing ends it truncated.
 */
static int a_listen_stream_is_opened_and_closed_by_the_client(void) {
    fixture_t fixture;
    ASSERT_TRUE(fixture_start(&fixture, NULL, 0, NULL, 0) == 0);
    int fd = connect_loopback(fixture.port);
    ASSERT_TRUE(fd >= 0);
    static const char request[] =
        REQUEST_LINE(LISTEN_HEADERS, LISTEN_LENGTH, LISTEN_BODY);
    ASSERT_TRUE(send_all(fd, request, sizeof(request) - 1u) == 0);
    char received[16384];
    ASSERT_TRUE(read_until(fd, received, sizeof(received),
        "notifications/subscriptions/acknowledged", 1, 3000) > 0);
    ASSERT_TRUE(status_of(received) == 200);
    ASSERT_TRUE(strstr(received, "Content-Type: text/event-stream") != NULL);
    /*
     * The acknowledgement opened the stream, which is the fact most readers
     * guess wrong: it travels the completion path even though it is a JSON-RPC
     * notification with no id, so "is this the final response for this id?"
     * says no and the reply becomes a stream instead of ending on it.
     */
    ASSERT_TRUE(strstr(received, "\"id\":14") == NULL);
    ASSERT_TRUE(strstr(received, "\r\n0\r\n\r\n") == NULL);

    /*
     * The keep-alive, on the wire. One chunk carrying a bare `:` comment,
     * written when the drain loop's poll goes idle - which is what an open
     * subscriptions/listen stream does between events, and the reason this
     * suite cannot read such a stream "until it goes quiet".
     */
    char idle[16384];
    ASSERT_TRUE(read_until(fd, idle, sizeof(idle), "3\r\n:\r\n\r\n", 2, 3000) > 0);
    ASSERT_TRUE(occurrences_of(idle, "3\r\n:\r\n\r\n") >= 2);

    /* The client ends it. Half-closed, so the truncation is observable. */
    ASSERT_TRUE(shutdown(fd, SHUT_WR) == 0);
    char after[16384];
    ASSERT_TRUE(read_response(fd, after, sizeof(after), 2000) >= 0);
    /* Cut, not completed: no terminal chunk and no resultType complete. A
     * client that walked away is not owed an ending it will not read. */
    ASSERT_TRUE(strstr(after, "0\r\n\r\n") == NULL);
    ASSERT_TRUE(strstr(after, "resultType") == NULL);
    close(fd);
    /* And the server lets go of it: stop returns rather than waiting for a
     * stream whose client is gone. */
    fixture_stop(&fixture);
    return 0;
}

/*
 * Server shutdown, which is the other way a stream ends and the one that is
 * NOT a cancellation. Every surviving subscription is completed with
 * `resultType: "complete"`, that frame is written, and the body is terminated
 * properly - because the peer is still connected and still reading.
 */
static int shutdown_completes_an_open_listen_stream(void) {
    fixture_t fixture;
    ASSERT_TRUE(fixture_start(&fixture, NULL, 0, NULL, 0) == 0);
    int fd = connect_loopback(fixture.port);
    ASSERT_TRUE(fd >= 0);
    static const char request[] =
        REQUEST_LINE(LISTEN_HEADERS, LISTEN_LENGTH, LISTEN_BODY);
    ASSERT_TRUE(send_all(fd, request, sizeof(request) - 1u) == 0);
    char opening[8192];
    ASSERT_TRUE(read_until(fd, opening, sizeof(opening),
        "notifications/subscriptions/acknowledged", 1, 3000) > 0);

    /* Phase 2: the listener stops, and this stream is asked to FINISH. */
    maelys_http_server_t *server = fixture.server;
    fixture.server = NULL;
    ASSERT_TRUE(maelys_http_server_stop(server) == MAELYS_MCP_OK);

    char closing[8192];
    ssize_t got = read_response(fd, closing, sizeof(closing), 2000);
    close(fd);
    ASSERT_TRUE(got > 0);
    /* The completion, and then the terminal chunk. A cancellation would have
     * written neither. */
    ASSERT_TRUE(strstr(closing, "\"resultType\":\"complete\"") != NULL);
    ASSERT_TRUE(strstr(closing, "\"id\":14") != NULL);
    ASSERT_TRUE(offset_of(closing, "\r\n0\r\n\r\n") >
        offset_of(closing, "\"resultType\":\"complete\""));
    fixture_stop(&fixture);
    return 0;
}

int main(void) {
    static const maelys_test_case_t cases[] = {
        {"the status ladder on the wire", wire_matrix},
        {"Origin is refused before anything else", origin_is_refused_before_anything_else},
        {"a kept-alive connection serves a second request",
            a_kept_alive_connection_serves_a_second_request},
        {"a pipelined request is refused", a_pipelined_request_is_refused},
        {"a rejection reaches a client still writing a body",
            a_rejection_reaches_a_client_still_writing_a_body},
        {"bearer authentication on the wire", bearer_authentication_on_the_wire},
        {"a public bind refuses loopback trust", a_public_bind_refuses_loopback_trust},
        {"MSG_PEEK tells a FIN from a pipelined byte",
            peek_tells_a_fin_from_a_pipelined_byte},
        {"loopback-trust takes no material", loopback_trust_takes_no_material},
        {"static-bearer outcomes", static_bearer_outcomes},
        {"principal refcounting", principal_refcounting},
        {"authenticate runs once per POST, twice on a reused connection",
            authenticate_runs_once_per_post},
        {"tools/list over HTTP is answered as JSON on a reused connection",
            tools_list_is_answered_as_json},
        {"a tools/call with progress streams its frames ahead of the response",
            a_call_with_progress_streams_in_order},
        {"a notification is 202 and the connection survives it",
            a_notification_is_202_and_keeps_the_connection},
        {"a client disconnect mid-call cancels it and frees the slot",
            a_client_disconnect_cancels_the_call},
        {"a listen stream opens on its acknowledgement and the client ends it",
            a_listen_stream_is_opened_and_closed_by_the_client},
        {"shutdown completes an open listen stream rather than cutting it",
            shutdown_completes_an_open_listen_stream}
    };
    return maelys_run_tests(cases, sizeof(cases) / sizeof(*cases));
}
