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
#include "maelys/mcp/http.h"
#include "tests/test_support.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * A body that satisfies H2's MCP rules, because since H2 the adapter validates
 * before it answers and a server-layer test that wanted to reach the adapter
 * would otherwise be refused by a rule it is not testing. It carries the
 * _meta protocol version the MCP-Protocol-Version header is compared against,
 * and its method matches the Mcp-Method header in STD_HEADERS.
 */
#define VALID_BODY \
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"params\":{\"_meta\":" \
    "{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"}}}"
#define VALID_BODY_LENGTH "116"
/* The Content-Length in the table below is written out, so it has to be pinned
 * to the body rather than counted by hand: a body one byte longer than its
 * declared length is indistinguishable from a pipelined request, and the test
 * would fail for a reason that has nothing to do with what it is testing. */
_Static_assert(sizeof(VALID_BODY) - 1u == 116u, "VALID_BODY is 116 bytes");

typedef struct fixture {
    maelys_mcp_authenticator_t authenticator;
    maelys_mcp_http_adapter_t *adapter;
    maelys_http_server_t *server;
    unsigned short port;
} fixture_t;

static int fixture_start(
    fixture_t *fixture,
    const char *const *tokens,
    size_t token_count,
    const char *const *origins,
    size_t origin_count) {
    memset(fixture, 0, sizeof(*fixture));
    maelys_mcp_result_t status = token_count
        ? maelys_http_auth_static_bearer_create(tokens, token_count, &fixture->authenticator)
        : maelys_http_auth_loopback_create(&fixture->authenticator);
    if (status != MAELYS_MCP_OK) return -1;
    if (maelys_mcp_http_adapter_create(
        MAELYS_MCP_HTTP_PLACEHOLDER_JSON, &fixture->adapter) != MAELYS_MCP_OK) {
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
    memset(counter, 0, sizeof(*counter));
    if (maelys_http_auth_loopback_create(&counter->inner) != MAELYS_MCP_OK) return -1;
    fixture->authenticator.name = "counting";
    fixture->authenticator.context = counter;
    fixture->authenticator.authenticate = counting_authenticate;
    fixture->authenticator.retain = counting_retain;
    fixture->authenticator.release = counting_release;
    fixture->authenticator.destroy = counting_destroy;
    if (maelys_mcp_http_adapter_create(
        MAELYS_MCP_HTTP_PLACEHOLDER_JSON, &fixture->adapter) != MAELYS_MCP_OK) {
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
        fixture->authenticator.destroy(fixture->authenticator.context);
        return -1;
    }
    fixture->port = maelys_http_server_port(fixture->server);
    return 0;
}

static void fixture_stop(fixture_t *fixture) {
    if (fixture->server) maelys_http_server_stop(fixture->server);
    if (fixture->adapter) maelys_mcp_http_adapter_destroy(fixture->adapter);
    if (fixture->authenticator.destroy) {
        fixture->authenticator.destroy(fixture->authenticator.context);
    }
    memset(fixture, 0, sizeof(*fixture));
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
 * reach the adapter needs them: leaving them out would make every 503 case fail
 * on a -32020 that has nothing to do with the ladder being tested.
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
    {"a well-formed POST reaches the adapter and gets its placeholder answer",
        "POST /mcp HTTP/1.1\r\n" STD_HEADERS "Content-Length: " VALID_BODY_LENGTH "\r\n\r\n" VALID_BODY,
        503, "-32600"},
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
        "Content-Length: " VALID_BODY_LENGTH "\r\n\r\n" VALID_BODY, 503, "-32600"},
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
        response, sizeof(response)) == 503);
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
        ASSERT_TRUE(status_of(response) == 503);
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
        response, sizeof(response)) == 503);
    ASSERT_TRUE(counter.calls == 1);

    /* One connection, two POSTs: exactly two more. */
    int fd = connect_loopback(fixture.port);
    ASSERT_TRUE(fd >= 0);
    for (int round = 0; round < 2; ++round) {
        ASSERT_TRUE(send_all(fd, request, strlen(request)) == 0);
        ssize_t got = read_one_message(fd, response, sizeof(response), 3000);
        ASSERT_TRUE(got > 0);
        ASSERT_TRUE(status_of(response) == 503);
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
        response, sizeof(response)) == 503);
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
    maelys_mcp_http_adapter_t *adapter = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_create(
        MAELYS_MCP_HTTP_PLACEHOLDER_JSON, &adapter) == MAELYS_MCP_OK);
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

    bearer.destroy(bearer.context);
    authenticator.destroy(authenticator.context);
    maelys_mcp_http_adapter_destroy(adapter);
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
            authenticate_runs_once_per_post}
    };
    return maelys_run_tests(cases, sizeof(cases) / sizeof(*cases));
}
