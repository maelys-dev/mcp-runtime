#include "host/http_server.h"

#include "host/http_auth.h"
#include "host/http_parser.h"
#include "maelys/mcp/runtime.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define READ_CHUNK_BYTES 4096u
#define DEFAULT_ENDPOINT_PATH "/mcp"
#define DEFAULT_BIND_ADDRESS "127.0.0.1"
/* One more than the endpoint the host can configure would ever need, and
 * bounded so the normalizer writes into a stack buffer rather than an
 * allocation whose size an attacker chooses. */
#define MAX_PATH_BYTES 1024u

typedef struct connection connection_t;

struct maelys_http_server {
    int listen_fd;
    /* The self-pipe that takes the acceptor out of poll() at stop. The pattern
     * is src/transport/stdio.c:69's, moved to a listener. */
    int wake_read;
    int wake_write;
    /*
     * Phase 2's signal, and one pipe for the whole server rather than one per
     * exchange: "finish gracefully" is a fact about the server and becomes true
     * for every in-flight exchange at the same instant. Its read end is handed
     * to the adapter as maelys_mcp_http_request_t::shutdown_fd, which only ever
     * polls it, so sharing one descriptor across every connection thread is
     * sound and costs two descriptors in total.
     */
    int shutdown_read;
    int shutdown_write;
    unsigned short port;
    int bind_is_loopback;

    pthread_t acceptor;
    int acceptor_started;

    /*
     * The transport's own state, behind one mutex that is NEVER held while a
     * runtime call is in progress. It is therefore a leaf and adds no edge to
     * the lock hierarchy in src/internal/internal.h:90.
     */
    pthread_mutex_t mutex;
    int stopping;
    size_t connection_count;
    /* How many connection threads are currently inside the adapter. Phase 2
     * waits on this and not on connection_count, because a connection parked
     * between two kept-alive requests has nothing to finish gracefully and
     * would only make the wait longer. */
    size_t exchange_count;
    connection_t *connections;

    /* Owned copies, so the caller's argv may go away. */
    char *endpoint_path;
    char **allowed_origins;
    size_t allowed_origin_count;

    size_t max_body_bytes;
    size_t max_connections;
    unsigned int header_timeout_ms;
    unsigned int body_timeout_ms;
    unsigned int idle_timeout_ms;
    unsigned int write_timeout_ms;
    const maelys_mcp_authenticator_t *authenticator;
    maelys_mcp_http_adapter_t *adapter;
};

struct connection {
    connection_t *next;
    maelys_http_server_t *server;
    pthread_t thread;
    int fd;
    int finished;
    char peer[INET6_ADDRSTRLEN + 8u];
};

/* --------------------------------------------------------------- utilities */

static int monotonic_milliseconds(uint64_t *out_value) {
    struct timespec now;
    if (!out_value || clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    *out_value = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
    return 0;
}

static uint64_t deadline_from(unsigned int timeout_ms) {
    uint64_t now = 0u;
    if (monotonic_milliseconds(&now) != 0) return 0u;
    return now + (uint64_t)timeout_ms;
}

static int poll_until(int fd, short events, uint64_t deadline_ms) {
    for (;;) {
        uint64_t now = 0u;
        if (monotonic_milliseconds(&now) != 0) return -1;
        if (now >= deadline_ms) return 0;
        uint64_t remaining = deadline_ms - now;
        int timeout = remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining;
        struct pollfd descriptor = {.fd = fd, .events = events, .revents = 0};
        int ready = poll(&descriptor, 1u, timeout);
        if (ready > 0) return 1;
        if (ready == 0) return 0;
        if (errno == EINTR) continue;
        return -1;
    }
}

static ssize_t read_deadline(int fd, void *buffer, size_t capacity, uint64_t deadline_ms) {
    for (;;) {
        int ready = poll_until(fd, POLLIN, deadline_ms);
        if (ready <= 0) return ready == 0 ? -2 : -1;
        ssize_t got = recv(fd, buffer, capacity, 0);
        if (got >= 0) return got;
        if (errno == EINTR) continue;
        return -1;
    }
}

static maelys_mcp_result_t write_all(int fd, const void *bytes, size_t length,
    unsigned int timeout_ms) {
    const char *cursor = bytes;
    uint64_t deadline = deadline_from(timeout_ms);
    while (length > 0) {
        int ready = poll_until(fd, POLLOUT, deadline);
        if (ready <= 0) return MAELYS_MCP_ERR_IO;
        ssize_t written;
#ifdef MSG_NOSIGNAL
        written = send(fd, cursor, length, MSG_NOSIGNAL);
#else
        written = send(fd, cursor, length, 0);
#endif
        if (written < 0) {
            if (errno == EINTR) continue;
            return MAELYS_MCP_ERR_IO;
        }
        if (written == 0) return MAELYS_MCP_ERR_IO;
        cursor += (size_t)written;
        length -= (size_t)written;
    }
    return MAELYS_MCP_OK;
}

/*
 * The connection's descriptor is closed here and nowhere else, under the
 * server's mutex and exactly once.
 *
 * That is not tidiness. maelys_http_server_stop walks the connection list and
 * shutdown()s each live socket to make an in-flight read return promptly; if a
 * connection thread could close its own descriptor outside the lock, stop could
 * be holding a number the kernel has already handed to something else. Closing
 * under the same lock stop takes, and clearing the field, removes the window
 * rather than narrowing it.
 */
static void connection_close(connection_t *connection) {
    pthread_mutex_lock(&connection->server->mutex);
    if (connection->fd >= 0) {
        close(connection->fd);
        connection->fd = -1;
    }
    pthread_mutex_unlock(&connection->server->mutex);
}

int maelys_http_peek_disambiguate(int fd) {
    /*
     * poll first, with a zero timeout, then MSG_PEEK. Not MSG_DONTWAIT: it is
     * a BSD/Linux extension that _POSIX_C_SOURCE hides on Darwin, and the
     * poll-then-peek pair is portable and says the same thing - once poll
     * reports the descriptor readable, a one-byte peek cannot block, because
     * either a byte or the FIN is already there.
     */
    for (;;) {
        struct pollfd descriptor = {.fd = fd, .events = POLLIN, .revents = 0};
        int ready = poll(&descriptor, 1u, 0);
        if (ready == 0) return -1;
        if (ready < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (!(descriptor.revents & (POLLIN | POLLHUP | POLLERR))) return -1;
        char byte = 0;
        ssize_t got = recv(fd, &byte, 1u, MSG_PEEK);
        if (got == 0) return 0;
        if (got > 0) return 1;
        if (errno == EINTR) continue;
        /* An error on the socket is not a pipelined byte and not a clean FIN.
         * Reporting it as neither leaves the exchange to fail on its own write,
         * which is where the error will surface with its own errno. */
        return -1;
    }
}

/* ---------------------------------------------------------------- responses */

typedef struct status_row {
    int status;
    const char *reason;
    /* The status table's JSON-RPC column. A -32600 with no id where the table
     * says so, and nothing at all where it says none. */
    int jsonrpc_error;
    const char *message;
    const char *extra_header;
} status_row_t;

static const status_row_t STATUS_ROWS[] = {
    /*
     * The one success row, and it is here for its REASON PHRASE rather than for
     * a body it never carries. writer_status_only reads this table to name the
     * status it is writing, and a 202 that was not in it went onto the wire as
     * "HTTP/1.1 202 Error" - syntactically legal, since a reason phrase is
     * advisory, and a lie to every human reading a capture.
     */
    {202, "Accepted", 0, NULL, NULL},
    {400, "Bad Request", 0, NULL, NULL},
    {401, "Unauthorized", -32600, "Authentication required.", "WWW-Authenticate: Bearer"},
    {403, "Forbidden", -32600, "Origin not allowed.", NULL},
    {404, "Not Found", 0, NULL, NULL},
    {405, "Method Not Allowed", 0, NULL, "Allow: POST"},
    {406, "Not Acceptable", -32600,
        "Accept must list application/json and text/event-stream.", NULL},
    {411, "Length Required", 0, NULL, NULL},
    {413, "Content Too Large", -32600, "Request body exceeds the configured limit.", NULL},
    {415, "Unsupported Media Type", -32600, "Content-Type must be application/json.", NULL},
    {417, "Expectation Failed", 0, NULL, NULL},
    {431, "Request Header Fields Too Large", 0, NULL, NULL},
    {503, "Service Unavailable", -32600, "The server cannot decide this request.", NULL},
    {505, "HTTP Version Not Supported", 0, NULL, NULL}
};

static const status_row_t *status_row(int status) {
    for (size_t index = 0; index < sizeof(STATUS_ROWS) / sizeof(*STATUS_ROWS); ++index) {
        if (STATUS_ROWS[index].status == status) return &STATUS_ROWS[index];
    }
    return NULL;
}

/*
 * Worst-case sizing rather than a guess: the format below can only produce the
 * fixed skeleton plus one status (3 digits), one reason phrase, one extra
 * header, one length (20 digits for a 64-bit size_t) and one message. Every
 * variable part is bounded here so the snprintf below cannot be the thing that
 * decides whether the buffer was big enough.
 */
#define RESPONSE_BUFFER_BYTES 1024u

static maelys_mcp_result_t send_status(
    int fd,
    unsigned int write_timeout_ms,
    const status_row_t *row) {
    char body[256];
    int body_length = 0;
    if (row->jsonrpc_error) {
        /* No id: a framing or transport refusal is not answering a request the
         * server ever parsed an id out of. */
        body_length = snprintf(body, sizeof(body),
            "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"%s\"}}",
            row->jsonrpc_error, row->message);
        if (body_length < 0 || (size_t)body_length >= sizeof(body)) return MAELYS_MCP_ERR_IO;
    }
    char head[RESPONSE_BUFFER_BYTES];
    int head_length = snprintf(head, sizeof(head),
        "HTTP/1.1 %d %s\r\n"
        "%s%s"
        "%s"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        row->status, row->reason,
        row->extra_header ? row->extra_header : "", row->extra_header ? "\r\n" : "",
        row->jsonrpc_error ? "Content-Type: application/json\r\n" : "",
        body_length);
    if (head_length < 0 || (size_t)head_length >= sizeof(head)) return MAELYS_MCP_ERR_IO;
    maelys_mcp_result_t status = write_all(fd, head, (size_t)head_length, write_timeout_ms);
    if (status == MAELYS_MCP_OK && body_length > 0) {
        status = write_all(fd, body, (size_t)body_length, write_timeout_ms);
    }
    return status;
}

/*
 * The RST-safe rejection close. Replying and closing immediately while the
 * client is still writing its body sends a TCP RST, and on most stacks the
 * client then loses the response it was supposed to read - so the caller sees a
 * connection error instead of an authentication failure, and retries.
 *
 * Write, shutdown the write half, discard a bounded amount under a short
 * deadline, close. That preserves what the authenticate-before-body ordering
 * exists for (no thread held for the body deadline, no body-sized allocation
 * for an unauthenticated caller) while still delivering the answer.
 */
static void reject_and_close(
    connection_t *connection,
    int status,
    int drain_body) {
    int fd = connection->fd;
    const status_row_t *row = status_row(status);
    if (row) (void)send_status(fd, connection->server->write_timeout_ms, row);
    if (drain_body) {
        (void)shutdown(fd, SHUT_WR);
        char scratch[READ_CHUNK_BYTES];
        size_t discarded = 0;
        uint64_t deadline = deadline_from(MAELYS_HTTP_REJECT_DISCARD_TIMEOUT_MS);
        /* At most 8 KiB, under a short deadline - not the body limit and not
         * the body deadline. An unauthenticated caller still cannot hold a
         * thread for the body deadline or cause a body-sized allocation. */
        while (discarded < MAELYS_HTTP_REJECT_DISCARD_BYTES) {
            size_t want = MAELYS_HTTP_REJECT_DISCARD_BYTES - discarded;
            if (want > sizeof(scratch)) want = sizeof(scratch);
            ssize_t got = read_deadline(fd, scratch, want, deadline);
            if (got <= 0) break;
            discarded += (size_t)got;
        }
    }
    connection_close(connection);
}

/* ------------------------------------------------------------ the writer */

/* The S-chain's watcher, defined below with the rest of it. The writer only
 * needs to tell it when the response is finished. */
typedef struct socket_watcher socket_watcher_t;
static void watcher_mark_finished(socket_watcher_t *watcher);

typedef struct socket_writer {
    connection_t *connection;
    int keep_alive;
    int began;
    int streamed;
    /* NULL when no watcher was started, which is the half-closed case. */
    socket_watcher_t *watcher;
} socket_writer_t;

/*
 * The instant the pipelining window closes: the response this exchange owes is
 * on the wire in full. Every terminal writer entry point ends here, and nothing
 * else does - a stream that has written an event is still mid-response.
 */
static void writer_response_complete(socket_writer_t *writer) {
    if (writer->watcher) watcher_mark_finished(writer->watcher);
}

static maelys_mcp_result_t writer_begin_json(
    void *context, int status, const char *body, size_t length) {
    socket_writer_t *writer = context;
    if (writer->began) return MAELYS_MCP_ERR_STATE;
    writer->began = 1;
    char head[RESPONSE_BUFFER_BYTES];
    int head_length = snprintf(head, sizeof(head),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status,
        status == 200 ? "OK" : (status_row(status) ? status_row(status)->reason : "Error"),
        length,
        writer->keep_alive ? "keep-alive" : "close");
    if (head_length < 0 || (size_t)head_length >= sizeof(head)) return MAELYS_MCP_ERR_IO;
    maelys_mcp_result_t result = write_all(writer->connection->fd, head,
        (size_t)head_length, writer->connection->server->write_timeout_ms);
    if (result == MAELYS_MCP_OK && length) {
        result = write_all(writer->connection->fd, body, length,
            writer->connection->server->write_timeout_ms);
    }
    writer_response_complete(writer);
    return result;
}

/*
 * An SSE reply is answered with Connection: close and the connection is closed
 * after the terminal chunk. Reuse after a long-lived stream saves nothing
 * measurable and asks the framing state machine to be right about a case that
 * would otherwise never be exercised.
 */
static maelys_mcp_result_t writer_begin_stream(void *context) {
    socket_writer_t *writer = context;
    if (writer->began) return MAELYS_MCP_ERR_STATE;
    writer->began = 1;
    writer->streamed = 1;
    writer->keep_alive = 0;
    static const char head[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-store\r\n"
        "X-Accel-Buffering: no\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n";
    return write_all(writer->connection->fd, head, sizeof(head) - 1u,
        writer->connection->server->write_timeout_ms);
}

/*
 * Transfer-Encoding: chunked is refused inbound and required outbound, and
 * there is no contradiction: the danger is ambiguity about what a REQUEST body
 * is, and a response the server itself frames has no such ambiguity.
 *
 * One chunk per SSE event, sized as "data: " + json + "\n\n" in lowercase hex
 * with no extensions, and flushed before the next frame is read.
 */
static maelys_mcp_result_t write_chunk(
    socket_writer_t *writer, const char *prefix, const char *payload,
    size_t payload_length, const char *suffix) {
    size_t prefix_length = prefix ? strlen(prefix) : 0u;
    size_t suffix_length = suffix ? strlen(suffix) : 0u;
    size_t total = prefix_length + payload_length + suffix_length;
    char head[32];
    int head_length = snprintf(head, sizeof(head), "%zx\r\n", total);
    if (head_length < 0 || (size_t)head_length >= sizeof(head)) return MAELYS_MCP_ERR_IO;
    int fd = writer->connection->fd;
    unsigned int timeout = writer->connection->server->write_timeout_ms;
    maelys_mcp_result_t result = write_all(fd, head, (size_t)head_length, timeout);
    if (result == MAELYS_MCP_OK && prefix_length) {
        result = write_all(fd, prefix, prefix_length, timeout);
    }
    if (result == MAELYS_MCP_OK && payload_length) {
        result = write_all(fd, payload, payload_length, timeout);
    }
    if (result == MAELYS_MCP_OK && suffix_length) {
        result = write_all(fd, suffix, suffix_length, timeout);
    }
    if (result == MAELYS_MCP_OK) result = write_all(fd, "\r\n", 2u, timeout);
    return result;
}

static maelys_mcp_result_t writer_write_event(
    void *context, const char *json, size_t length) {
    socket_writer_t *writer = context;
    if (!writer->streamed) return MAELYS_MCP_ERR_STATE;
    return write_chunk(writer, "data: ", json, length, "\n\n");
}

static maelys_mcp_result_t writer_write_keepalive(void *context) {
    socket_writer_t *writer = context;
    if (!writer->streamed) return MAELYS_MCP_ERR_STATE;
    return write_chunk(writer, NULL, ":\r\n", 3u, NULL);
}

static maelys_mcp_result_t writer_end_stream(
    void *context, maelys_mcp_http_stream_end_t disposition) {
    socket_writer_t *writer = context;
    if (!writer->streamed) return MAELYS_MCP_ERR_STATE;
    writer->streamed = 0;
    /*
     * ABORTED emits no further bytes at all. A chunked body that ends without
     * its terminal chunk is exactly how HTTP/1.1 says "this response is
     * truncated"; writing 0\r\n\r\n and then closing would tell the client the
     * stream completed normally, which would silently turn a cancelled call
     * into an apparently-successful empty one.
     */
    if (disposition == MAELYS_MCP_HTTP_STREAM_ABORTED) {
        writer_response_complete(writer);
        return MAELYS_MCP_OK;
    }
    maelys_mcp_result_t result = write_all(writer->connection->fd, "0\r\n\r\n", 5u,
        writer->connection->server->write_timeout_ms);
    writer_response_complete(writer);
    return result;
}

/*
 * A status-only reply, which since H3 is how a notification's 202 is written.
 *
 * Keep-alive is NOT forced off here, and that changed with H3 for a reason: the
 * body length is explicitly zero, so the connection is left in a known state
 * and the framing state machine has nothing to guess. Refusing reuse after a
 * 202 would make a client that batches notifications on one connection pay a
 * handshake per notification, for no property anyone gains.
 */
static maelys_mcp_result_t writer_status_only(
    void *context, int status, const char *const *extra_headers,
    size_t extra_header_count) {
    socket_writer_t *writer = context;
    if (writer->began) return MAELYS_MCP_ERR_STATE;
    writer->began = 1;
    const status_row_t *row = status_row(status);
    char head[RESPONSE_BUFFER_BYTES];
    int head_length = snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\n",
        status, row ? row->reason : "Error");
    if (head_length < 0 || (size_t)head_length >= sizeof(head)) return MAELYS_MCP_ERR_IO;
    int fd = writer->connection->fd;
    unsigned int timeout = writer->connection->server->write_timeout_ms;
    maelys_mcp_result_t result = write_all(fd, head, (size_t)head_length, timeout);
    for (size_t index = 0; result == MAELYS_MCP_OK && index < extra_header_count; ++index) {
        if (!extra_headers[index]) continue;
        result = write_all(fd, extra_headers[index], strlen(extra_headers[index]), timeout);
        if (result == MAELYS_MCP_OK) result = write_all(fd, "\r\n", 2u, timeout);
    }
    if (result == MAELYS_MCP_OK) {
        const char *tail = writer->keep_alive
            ? "Content-Length: 0\r\nConnection: keep-alive\r\n\r\n"
            : "Content-Length: 0\r\nConnection: close\r\n\r\n";
        result = write_all(fd, tail, strlen(tail), timeout);
    }
    writer_response_complete(writer);
    return result;
}

/* ---------------------------------------------------------- the S-chain */

/*
 * S0-S3. The socket watcher: the one party that looks at the socket while an
 * exchange is being produced, and the whole of what the adapter is spared.
 *
 * It exists because the two jobs cannot share a thread. The adapter's drain
 * sits in poll() on descriptors that are not sockets and must not be, since
 * telling a FIN from a pipelined byte needs the socket AND the pipelining rule
 * AND the ability to say which of the two happened - none of which the adapter
 * has or should have. So the connection thread runs the exchange and this
 * thread watches the socket beside it.
 *
 * The lifetime protocol is a pipe rather than a reference, which is what makes
 * it small: this thread never names the exchange, never touches the channel,
 * and never aborts anything. It writes one byte. The connection thread creates
 * the pipe before this thread exists and closes it after joining, so there is
 * no window in which this thread holds a descriptor number that has been handed
 * to something else.
 *
 * Both endings write that byte, and the two are DIFFERENT FAULTS logged
 * differently even though they end the same way. Conflating a client bug with a
 * cancellation at the source is how a pipelining defect disappears into a
 * disconnect statistic.
 */
/* Defined with the listener below, where the acceptor's own wakeup pipe is. */
static int create_wakeup_pipe(int *read_fd, int *write_fd);

struct socket_watcher {
    pthread_t thread;
    int started;
    int fd;
    /* Read end handed to the adapter as cancel_fd; write end raised here. */
    int cancel_read;
    int cancel_write;
    const char *peer;
    /*
     * Set by the connection thread the moment the exchange is over, and read by
     * the watcher before it decides that an inbound byte means anything.
     *
     * Without it a kept-alive client that sends its NEXT request promptly is
     * reported as pipelining: the byte is legitimate, because this request's
     * response was already complete, but the watcher is still in poll() and
     * cannot tell that from the socket alone. The pipelining rule is about
     * bytes arriving BETWEEN a request's body and its response, and this flag
     * is what makes the watcher's view of "between" the same as the rule's.
     */
    pthread_mutex_t mutex;
    int mutex_ready;
    int finished;
};

static void watcher_mark_finished(socket_watcher_t *watcher) {
    if (!watcher->mutex_ready) return;
    pthread_mutex_lock(&watcher->mutex);
    watcher->finished = 1;
    pthread_mutex_unlock(&watcher->mutex);
}

static void watcher_raise(socket_watcher_t *watcher) {
    if (watcher->cancel_write < 0) return;
    ssize_t written = write(watcher->cancel_write, "x", 1u);
    (void)written;
}

static int watcher_finished(socket_watcher_t *watcher) {
    pthread_mutex_lock(&watcher->mutex);
    int finished = watcher->finished;
    pthread_mutex_unlock(&watcher->mutex);
    return finished;
}

static void *watcher_main(void *argument) {
    socket_watcher_t *watcher = argument;
    for (;;) {
        struct pollfd descriptors[2] = {
            {.fd = watcher->fd, .events = POLLIN, .revents = 0},
            /* Readable means somebody already ended this exchange - the
             * connection thread on its way out, or a shutdown - and there is
             * nothing left to watch for. Polled, never read: the adapter's
             * level-triggered reading of the same descriptor depends on the
             * byte staying there. */
            {.fd = watcher->cancel_read, .events = POLLIN, .revents = 0}
        };
        int ready = poll(descriptors, 2u, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return NULL;
        }
        if (descriptors[1].revents) return NULL;
        if (!descriptors[0].revents) continue;
        int peeked = maelys_http_peek_disambiguate(watcher->fd);
        /* Checked AFTER the peek and before acting on it: an exchange that
         * ended while this thread was in poll() has nothing left to cancel, and
         * whatever the peer sent belongs to its next request. */
        if (watcher_finished(watcher)) return NULL;
        if (peeked == 0) {
            /* FIN. On this transport the client closing IS the cancellation. */
            watcher_raise(watcher);
            return NULL;
        }
        if (peeked == 1) {
            fprintf(stderr,
                "http: refused pipelined bytes from %s during an exchange\n",
                watcher->peer);
            watcher_raise(watcher);
            return NULL;
        }
        /* Neither yet: a spurious wakeup, or an error that the exchange's own
         * write will surface with its own errno. Poll again rather than guess,
         * because guessing here would abort a live request. */
        struct pollfd settle = {.fd = watcher->fd, .events = POLLIN, .revents = 0};
        if (poll(&settle, 1u, 1) < 0 && errno != EINTR) return NULL;
    }
}

static int watcher_start(socket_watcher_t *watcher, int fd, const char *peer) {
    memset(watcher, 0, sizeof(*watcher));
    watcher->fd = fd;
    watcher->peer = peer;
    watcher->cancel_read = -1;
    watcher->cancel_write = -1;
    if (pthread_mutex_init(&watcher->mutex, NULL) != 0) return -1;
    watcher->mutex_ready = 1;
    if (create_wakeup_pipe(&watcher->cancel_read, &watcher->cancel_write) != 0) {
        return -1;
    }
    if (pthread_create(&watcher->thread, NULL, watcher_main, watcher) != 0) {
        close(watcher->cancel_read);
        close(watcher->cancel_write);
        watcher->cancel_read = -1;
        watcher->cancel_write = -1;
        return -1;
    }
    watcher->started = 1;
    return 0;
}

static void watcher_stop(socket_watcher_t *watcher) {
    if (watcher->started) {
        pthread_mutex_lock(&watcher->mutex);
        watcher->finished = 1;
        pthread_mutex_unlock(&watcher->mutex);
        /* Raised by this thread too, so the watcher leaves poll() whether or
         * not the client did anything. Joined before either end is closed. */
        watcher_raise(watcher);
        pthread_join(watcher->thread, NULL);
        watcher->started = 0;
    }
    if (watcher->cancel_read >= 0) close(watcher->cancel_read);
    if (watcher->cancel_write >= 0) close(watcher->cancel_write);
    watcher->cancel_read = -1;
    watcher->cancel_write = -1;
    if (watcher->mutex_ready) {
        pthread_mutex_destroy(&watcher->mutex);
        watcher->mutex_ready = 0;
    }
}

/* ------------------------------------------------------------ the exchange */

typedef struct header_lookup_context {
    const maelys_http_parser_t *parser;
} header_lookup_context_t;

static int header_lookup(
    void *context, const char *name, const char **out_value, size_t *out_length) {
    const header_lookup_context_t *lookup = context;
    return maelys_http_parser_header(lookup->parser, name, out_value, out_length);
}

/*
 * The credential, built FROM HEADERS ONLY. There is nothing in
 * maelys_mcp_transport_credentials_t that could hold a body view, which is what
 * makes the ordering below a contract rather than a convention: the credential
 * is checked as soon as the headers are complete, and before a single byte of
 * request body is read.
 *
 * Authorization is handed across whole and unparsed. Stripping the scheme is
 * the authenticator's job, because the transport has no opinion about what a
 * credential looks like.
 */
static maelys_mcp_result_t authenticate_request(
    connection_t *connection,
    const maelys_http_parser_t *parser,
    maelys_mcp_principal_t **out_principal) {
    const maelys_mcp_authenticator_t *authenticator = connection->server->authenticator;
    const char *authorization = NULL;
    size_t authorization_length = 0;
    maelys_mcp_transport_credentials_t credentials = {
        .kind = MAELYS_MCP_CREDENTIAL_NONE,
        .peer = connection->peer,
        .material = NULL,
        .material_length = 0
    };
    if (maelys_http_parser_header(parser, "authorization",
        &authorization, &authorization_length)) {
        credentials.kind = MAELYS_MCP_CREDENTIAL_BEARER;
        credentials.material = authorization;
        credentials.material_length = authorization_length;
    }
    return authenticator->authenticate(authenticator->context, &credentials, out_principal);
}

/*
 * The validation ladder, in the order the design's flow fixes, with two
 * orderings the flow leaves implicit and this comment makes explicit.
 *
 * Origin runs first because it is the DNS-rebinding control and the one check
 * that must run earliest - before the body is read, before authentication, and
 * before any channel exists.
 *
 * Method precedes the media checks. The flow diagram lists Content-Type and
 * Accept above routing, but a GET carries neither, and answering a GET 415
 * instead of the 405 the methods table promises would be wrong; so path
 * routing (404) and then the method (405) come first, and only a POST to this
 * endpoint is ever asked about its media types.
 *
 * Returns 0 to continue, or an HTTP status to reject with.
 */
static int validate_request(
    connection_t *connection,
    const maelys_http_parser_t *parser,
    char *path,
    size_t path_capacity) {
    maelys_http_server_t *server = connection->server;
    const char *host = NULL;
    size_t host_length = 0;
    if (!maelys_http_parser_header(parser, "host", &host, &host_length)) return 400;
    if (!maelys_http_host_valid(host, host_length, server->bind_is_loopback)) return 400;

    const char *origin = NULL;
    size_t origin_length = 0;
    int has_origin = maelys_http_parser_header(parser, "origin", &origin, &origin_length);
    if (!maelys_http_origin_allowed(has_origin ? origin : NULL, origin_length,
        (const char *const *)server->allowed_origins, server->allowed_origin_count,
        server->bind_is_loopback)) {
        return 403;
    }

    size_t target_length = 0;
    const char *target = maelys_http_parser_target(parser, &target_length);
    if (!maelys_http_target_normalize(target, target_length, host, host_length,
        path, path_capacity)) {
        return 400;
    }
    if (strcmp(path, server->endpoint_path) != 0) return 404;

    size_t method_length = 0;
    const char *method = maelys_http_parser_method(parser, &method_length);
    if (method_length != 4u || memcmp(method, "POST", 4u) != 0) return 405;

    /* 100-continue is the standard answer to the problem authenticate-before-body
     * creates, and it is out of v1 only because it adds a second inbound state
     * to a parser whose smallness is the security argument. */
    if (maelys_http_parser_header(parser, "expect", NULL, NULL)) return 417;

    /* No implicit zero-length body. */
    if (!parser->has_content_length) return 411;
    if (parser->content_length > server->max_body_bytes) return 413;

    const char *content_type = NULL;
    size_t content_type_length = 0;
    if (!maelys_http_parser_header(parser, "content-type",
        &content_type, &content_type_length) ||
        !maelys_http_content_type_json(content_type, content_type_length)) {
        return 415;
    }

    const char *accept = NULL;
    size_t accept_length = 0;
    if (!maelys_http_parser_header(parser, "accept", &accept, &accept_length) ||
        !maelys_http_accept_sufficient(accept, accept_length)) {
        return 406;
    }
    return 0;
}

static int client_asked_to_close(const maelys_http_parser_t *parser) {
    const char *value = NULL;
    size_t length = 0;
    if (!maelys_http_parser_header(parser, "connection", &value, &length)) return 0;
    /* A comma list, because "close, keep-alive" is expressible even if it is
     * not sensible; any mention of close closes. */
    for (size_t index = 0; index + 5u <= length; ++index) {
        if (strncasecmp(value + index, "close", 5u) == 0) return 1;
    }
    return 0;
}

static int reject_status_for(maelys_http_reject_t reject) {
    switch (reject) {
        case MAELYS_HTTP_REJECT_HEADERS_TOO_LARGE: return 431;
        case MAELYS_HTTP_REJECT_VERSION: return 505;
        default: return 400;
    }
}

/*
 * One request on one connection. Returns 1 when the connection may be reused,
 * 0 when it has been closed.
 *
 * The socket is closed by whichever path ends the exchange, never by the
 * caller, so that every rejection can choose between the immediate close a
 * framing error demands and the RST-safe close a rejected caller needs.
 */
static int serve_request(connection_t *connection, int is_first_request) {
    maelys_http_server_t *server = connection->server;
    maelys_http_parser_t parser;
    maelys_http_parser_init(&parser);

    uint64_t deadline = deadline_from(is_first_request
        ? server->header_timeout_ms : server->idle_timeout_ms);
    int saw_first_byte = 0;
    maelys_http_parse_state_t state = MAELYS_HTTP_PARSE_INCOMPLETE;
    while (state == MAELYS_HTTP_PARSE_INCOMPLETE) {
        char chunk[READ_CHUNK_BYTES];
        ssize_t got = read_deadline(connection->fd, chunk, sizeof(chunk), deadline);
        if (got <= 0) {
            /* A clean close between requests is not an error; a timeout or a
             * half-request is, and either way nothing is written back. */
            maelys_http_parser_clear(&parser);
            connection_close(connection);
            return 0;
        }
        if (!saw_first_byte) {
            saw_first_byte = 1;
            deadline = deadline_from(server->header_timeout_ms);
        }
        state = maelys_http_parser_feed(&parser, chunk, (size_t)got);
    }
    if (state == MAELYS_HTTP_PARSE_REJECTED) {
        if (parser.reject == MAELYS_HTTP_REJECT_SMUGGLING) {
            /* Its own signal, deliberately, even though the blanket
             * Transfer-Encoding refusal already covered it: this is the single
             * most exploited desync primitive and a log line calling it
             * "malformed" buries it. */
            fprintf(stderr,
                "http: refused a request-smuggling attempt from %s "
                "(Transfer-Encoding with Content-Length)\n", connection->peer);
        }
        /* A framing error never drains and never resynchronizes: the
         * connection's byte stream has stopped meaning anything. */
        reject_and_close(connection, reject_status_for(parser.reject), 0);
        maelys_http_parser_clear(&parser);
        return 0;
    }

    char path[MAX_PATH_BYTES];
    int rejection = validate_request(connection, &parser, path, sizeof(path));
    /* The client may still be writing a body, so every rejection past the parse
     * takes the RST-safe close - not only the 401 the principal document names
     * it for. The hazard is the same for a 413 or a 415. */
    int drain = parser.has_content_length && parser.content_length > 0;
    if (rejection) {
        maelys_http_parser_clear(&parser);
        reject_and_close(connection, rejection, drain);
        return 0;
    }

    maelys_mcp_principal_t *principal = NULL;
    maelys_mcp_result_t auth = authenticate_request(connection, &parser, &principal);
    if (auth != MAELYS_MCP_OK) {
        /*
         * Absent and invalid are both 401 with a bearer challenge; anything
         * else is 503. An authenticator whose backend is unreachable has not
         * established that the caller is anonymous and has not established that
         * it is forbidden, and mapping that to 401 would silently downgrade
         * every request to unauthenticated during an outage.
         */
        int status = (auth == MAELYS_MCP_ERR_DENIED || auth == MAELYS_MCP_ERR_ARGUMENT)
            ? 401 : 503;
        maelys_http_parser_clear(&parser);
        reject_and_close(connection, status, drain);
        return 0;
    }

    /* Only now the body. */
    char *body = NULL;
    size_t body_length = 0;
    size_t residue_length = 0;
    const char *residue = maelys_http_parser_residue(&parser, &residue_length);
    if (parser.content_length > 0) {
        body = malloc(parser.content_length);
        if (!body) {
            server->authenticator->release(server->authenticator->context, principal);
            maelys_http_parser_clear(&parser);
            reject_and_close(connection, 503, 1);
            return 0;
        }
    }
    size_t copied = residue_length < parser.content_length
        ? residue_length : parser.content_length;
    if (copied) memcpy(body, residue, copied);
    body_length = copied;
    uint64_t body_deadline = deadline_from(server->body_timeout_ms);
    while (body_length < parser.content_length) {
        ssize_t got = read_deadline(connection->fd, body + body_length,
            parser.content_length - body_length, body_deadline);
        if (got <= 0) {
            free(body);
            server->authenticator->release(server->authenticator->context, principal);
            maelys_http_parser_clear(&parser);
            connection_close(connection);
            return 0;
        }
        body_length += (size_t)got;
    }

    /*
     * S0-S3. Pipelining is never permitted, and refusing it is not only a
     * smuggling defence: it is what makes the disconnect detector sound, by
     * letting "readable socket" mean "the peer went away" rather than "the peer
     * went away OR sent something".
     *
     * Two shapes reach here. Bytes past this request's body that the header
     * read already buffered are a pipelined request outright. Bytes still on
     * the socket are told apart from a FIN by MSG_PEEK, because POLLIN fires
     * for both and conflating them would hide a client bug behind a
     * cancellation.
     */
    int pipelined = residue_length > parser.content_length;
    int half_closed = 0;
    if (!pipelined) {
        int peeked = maelys_http_peek_disambiguate(connection->fd);
        if (peeked == 1) pipelined = 1;
        /*
         * A FIN that is ALREADY here when the request is complete is a client
         * that has finished asking and is still reading - a half-close, which
         * is legitimate and is answered. It is deliberately not the
         * cancellation reading: cancellation is a FIN that arrives WHILE the
         * answer is being produced, and telling the two apart is exactly what
         * this peek, before the watcher starts, is for. Starting a watcher on a
         * socket that is already at end-of-stream would abort every half-closing
         * client's request the instant it was dispatched.
         */
        if (peeked == 0) half_closed = 1;
    }
    if (pipelined) {
        fprintf(stderr, "http: refused pipelined bytes from %s\n", connection->peer);
        free(body);
        server->authenticator->release(server->authenticator->context, principal);
        maelys_http_parser_clear(&parser);
        connection_close(connection);
        return 0;
    }

    /*
     * The watcher, and only when there is something left to watch for. A
     * half-closed peer has already said everything it is going to say, so the
     * socket is permanently readable and a watcher on it would report a
     * cancellation that has not happened.
     *
     * A watcher that cannot start is not fatal: the exchange runs with no
     * cancellation source, which is the same contract an embedder that offers
     * neither gets, and _handle answers accordingly rather than pretending.
     */
    socket_watcher_t watcher;
    memset(&watcher, 0, sizeof(watcher));
    watcher.cancel_read = -1;
    watcher.cancel_write = -1;
    if (!half_closed) {
        (void)watcher_start(&watcher, connection->fd, connection->peer);
    }

    header_lookup_context_t lookup = {.parser = &parser};
    socket_writer_t writer_state = {
        .connection = connection,
        /* Keep-alive is permitted for JSON-mode replies and refused for event
         * streams; writer_begin_stream clears it. */
        .keep_alive = !half_closed && !client_asked_to_close(&parser),
        .began = 0,
        .streamed = 0,
        /*
         * The writer is what knows when the response is COMPLETE, which is
         * where the pipelining window closes. Not when _handle returns: the
         * channel is still being torn down then, and a kept-alive client that
         * reads its answer and sends the next request promptly would be
         * reported as pipelining for bytes that arrived after the response the
         * rule measures against.
         */
        .watcher = &watcher
    };
    maelys_mcp_http_response_writer_t writer = {
        .context = &writer_state,
        .begin_json = writer_begin_json,
        .begin_stream = writer_begin_stream,
        .write_event = writer_write_event,
        .write_keepalive = writer_write_keepalive,
        .end_stream = writer_end_stream,
        .status_only = writer_status_only
    };

    maelys_mcp_http_request_t request = {
        .method = "POST",
        .path = path,
        .header_lookup = header_lookup,
        .header_context = &lookup,
        .body = body,
        .body_length = body_length,
        .principal = principal,
        /*
         * The retain/release pair, so the channel gets a reference of its own.
         * The server layer keeps the one it authenticated and gives it back
         * below; the adapter's goes back through
         * maelys_mcp_channel_config_t::context_release, at the real free, which
         * for a detached channel is later than this function and on a thread
         * this one never created. That is the whole reason ABI 4 grew a context
         * destructor, and this is its first consumer.
         */
        .principal_retain = server->authenticator->retain,
        .principal_release = server->authenticator->release,
        .principal_context = server->authenticator->context,
        /* Abstract on the adapter's side; a pipe the watcher raises on this
         * one. The adapter never learns whether it was a FIN or a pipelined
         * byte, because it does not need to and could not act differently. */
        .cancel_fd = watcher.cancel_read,
        .shutdown_fd = server->shutdown_read
    };
    pthread_mutex_lock(&server->mutex);
    ++server->exchange_count;
    pthread_mutex_unlock(&server->mutex);
    maelys_mcp_result_t handled = maelys_mcp_http_adapter_handle(
        server->adapter, &request, &writer, NULL);
    pthread_mutex_lock(&server->mutex);
    --server->exchange_count;
    pthread_mutex_unlock(&server->mutex);

    watcher_stop(&watcher);
    free(body);
    /*
     * The server layer's own reference, released here as it always was. The
     * channel's second one is not this call: releasing twice from here would
     * be exactly the double free the two-reference shape exists to avoid.
     */
    server->authenticator->release(server->authenticator->context, principal);
    maelys_http_parser_clear(&parser);

    if (handled != MAELYS_MCP_OK || !writer_state.keep_alive) {
        connection_close(connection);
        return 0;
    }
    return 1;
}

static void *connection_main(void *argument) {
    connection_t *connection = argument;
    int reuse = 1;
    for (int index = 0; reuse; ++index) {
        int stopping = 0;
        pthread_mutex_lock(&connection->server->mutex);
        stopping = connection->server->stopping;
        pthread_mutex_unlock(&connection->server->mutex);
        if (stopping) {
            connection_close(connection);
            break;
        }
        reuse = serve_request(connection, index == 0);
    }
    pthread_mutex_lock(&connection->server->mutex);
    connection->finished = 1;
    --connection->server->connection_count;
    pthread_mutex_unlock(&connection->server->mutex);
    return NULL;
}

/* ------------------------------------------------------------- the listener */

/* Joins and frees every connection whose thread has already returned. Called
 * from the acceptor between accepts, so a finished connection stops counting
 * against the limit promptly, and from stop for the rest. */
static void reap_connections(maelys_http_server_t *server, int drain_all) {
    connection_t *harvested = NULL;
    pthread_mutex_lock(&server->mutex);
    connection_t **cursor = &server->connections;
    while (*cursor) {
        connection_t *entry = *cursor;
        if (drain_all || entry->finished) {
            *cursor = entry->next;
            entry->next = harvested;
            harvested = entry;
        } else {
            cursor = &entry->next;
        }
    }
    pthread_mutex_unlock(&server->mutex);
    /* Never joined under the mutex: the thread being joined takes that same
     * mutex on its way out. */
    while (harvested) {
        connection_t *entry = harvested;
        harvested = entry->next;
        pthread_join(entry->thread, NULL);
        free(entry);
    }
}

static void describe_peer(int fd, char *out, size_t capacity) {
    struct sockaddr_storage address;
    socklen_t length = sizeof(address);
    if (getpeername(fd, (struct sockaddr *)&address, &length) != 0) {
        snprintf(out, capacity, "unknown");
        return;
    }
    char host[INET6_ADDRSTRLEN] = {0};
    if (address.ss_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)&address;
        if (!inet_ntop(AF_INET, &v4->sin_addr, host, sizeof(host))) {
            snprintf(out, capacity, "unknown");
            return;
        }
        snprintf(out, capacity, "%s:%u", host, (unsigned int)ntohs(v4->sin_port));
        return;
    }
    const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&address;
    if (!inet_ntop(AF_INET6, &v6->sin6_addr, host, sizeof(host))) {
        snprintf(out, capacity, "unknown");
        return;
    }
    snprintf(out, capacity, "[%s]:%u", host, (unsigned int)ntohs(v6->sin6_port));
}

static void *acceptor_main(void *argument) {
    maelys_http_server_t *server = argument;
    for (;;) {
        reap_connections(server, 0);
        struct pollfd descriptors[2] = {
            {.fd = server->listen_fd, .events = POLLIN, .revents = 0},
            {.fd = server->wake_read, .events = POLLIN, .revents = 0}
        };
        int ready = poll(descriptors, 2u, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (descriptors[1].revents) break;
        if (!(descriptors[0].revents & POLLIN)) continue;
        int fd = accept(server->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno == EMFILE || errno == ENFILE) continue;
            break;
        }
#ifdef SO_NOSIGPIPE
        int on = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
        int stopping = 0;
        int at_capacity = 0;
        pthread_mutex_lock(&server->mutex);
        stopping = server->stopping;
        if (!stopping) {
            /* The concurrency bound in v1 is the connection limit; there is no
             * thread pool, so this is the only thing standing between the
             * listener and a thread per attacker. */
            at_capacity = server->connection_count >= server->max_connections;
            if (!at_capacity) ++server->connection_count;
        }
        pthread_mutex_unlock(&server->mutex);
        if (stopping || at_capacity) {
            /*
             * Closed without a reply. Writing a 503 here would put a socket
             * write on the acceptor thread, which is exactly the thread a
             * slow-reading peer must never be able to stall - the connection
             * limit is the defence, and paying for it with a stalled acceptor
             * would hand the attacker the listener instead of one slot.
             */
            close(fd);
            continue;
        }
        connection_t *connection = calloc(1u, sizeof(*connection));
        if (!connection) {
            pthread_mutex_lock(&server->mutex);
            --server->connection_count;
            pthread_mutex_unlock(&server->mutex);
            close(fd);
            continue;
        }
        connection->server = server;
        connection->fd = fd;
        describe_peer(fd, connection->peer, sizeof(connection->peer));
        pthread_mutex_lock(&server->mutex);
        connection->next = server->connections;
        server->connections = connection;
        pthread_mutex_unlock(&server->mutex);
        if (pthread_create(&connection->thread, NULL, connection_main, connection) != 0) {
            pthread_mutex_lock(&server->mutex);
            server->connections = connection->next;
            --server->connection_count;
            pthread_mutex_unlock(&server->mutex);
            close(fd);
            free(connection);
        }
    }
    return NULL;
}

static void server_free(maelys_http_server_t *server) {
    if (!server) return;
    if (server->listen_fd >= 0) close(server->listen_fd);
    if (server->wake_read >= 0) close(server->wake_read);
    if (server->wake_write >= 0) close(server->wake_write);
    if (server->shutdown_read >= 0) close(server->shutdown_read);
    if (server->shutdown_write >= 0) close(server->shutdown_write);
    free(server->endpoint_path);
    for (size_t index = 0; index < server->allowed_origin_count; ++index) {
        free(server->allowed_origins[index]);
    }
    free(server->allowed_origins);
    pthread_mutex_destroy(&server->mutex);
    free(server);
}

static int create_wakeup_pipe(int *read_fd, int *write_fd) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    for (size_t index = 0; index < 2u; ++index) {
        int flags = fcntl(fds[index], F_GETFD);
        if (flags < 0 || fcntl(fds[index], F_SETFD, flags | FD_CLOEXEC) != 0) {
            close(fds[0]);
            close(fds[1]);
            return -1;
        }
    }
    int flags = fcntl(fds[1], F_GETFL);
    if (flags < 0 || fcntl(fds[1], F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    *read_fd = fds[0];
    *write_fd = fds[1];
    return 0;
}

/*
 * Resolve the bind address without creating a socket, so that the
 * loopback-only refusal below can happen BEFORE anything is listening. A check
 * that binds first and then refuses has already exposed the port, however
 * briefly, which is the one moment the check exists to prevent.
 */
static maelys_mcp_result_t resolve_bind(
    const char *address,
    unsigned short port,
    struct sockaddr_storage *storage,
    socklen_t *out_length,
    int *out_family,
    int *out_loopback) {
    int family = strchr(address, ':') ? AF_INET6 : AF_INET;
    memset(storage, 0, sizeof(*storage));
    if (family == AF_INET) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)storage;
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        if (inet_pton(AF_INET, address, &v4->sin_addr) != 1) return MAELYS_MCP_ERR_ARGUMENT;
        /* 127.0.0.0/8 in full, not only 127.0.0.1. */
        *out_loopback = (ntohl(v4->sin_addr.s_addr) & 0xff000000u) == 0x7f000000u;
        *out_length = sizeof(*v4);
    } else {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)storage;
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        if (inet_pton(AF_INET6, address, &v6->sin6_addr) != 1) return MAELYS_MCP_ERR_ARGUMENT;
        *out_loopback = IN6_IS_ADDR_LOOPBACK(&v6->sin6_addr) ? 1 : 0;
        *out_length = sizeof(*v6);
    }
    *out_family = family;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t bind_listener(
    maelys_http_server_t *server,
    const struct sockaddr_storage *resolved,
    socklen_t length,
    int family) {
    struct sockaddr_storage storage = *resolved;
    server->listen_fd = socket(family, SOCK_STREAM, 0);
    if (server->listen_fd < 0) return MAELYS_MCP_ERR_IO;
    int flags = fcntl(server->listen_fd, F_GETFD);
    if (flags < 0 || fcntl(server->listen_fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        return MAELYS_MCP_ERR_IO;
    }
    int on = 1;
    (void)setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (family == AF_INET6) {
        /* No dual-stack surprise: a listener asked for ::1 binds ::1 and not
         * also every IPv4 address. */
        (void)setsockopt(server->listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
    }
    if (bind(server->listen_fd, (struct sockaddr *)&storage, length) != 0) {
        return MAELYS_MCP_ERR_IO;
    }
    if (listen(server->listen_fd, 64) != 0) return MAELYS_MCP_ERR_IO;
    socklen_t bound_length = sizeof(storage);
    if (getsockname(server->listen_fd, (struct sockaddr *)&storage, &bound_length) != 0) {
        return MAELYS_MCP_ERR_IO;
    }
    server->port = family == AF_INET
        ? ntohs(((struct sockaddr_in *)&storage)->sin_port)
        : ntohs(((struct sockaddr_in6 *)&storage)->sin6_port);
    return MAELYS_MCP_OK;
}

maelys_mcp_result_t maelys_http_server_start(
    const maelys_http_server_options_t *options,
    maelys_http_server_t **out_server) {
    if (!out_server || !options || !options->adapter) return MAELYS_MCP_ERR_ARGUMENT;
    /* An authenticator is not optional. A listener with no authentication
     * boundary has no principal to bind, and "whoever can reach this socket" is
     * a claim that has to be made explicitly rather than by omission. */
    if (!options->authenticator || !options->authenticator->authenticate ||
        !options->authenticator->retain || !options->authenticator->release) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    maelys_http_server_t *server = calloc(1u, sizeof(*server));
    if (!server) return MAELYS_MCP_ERR_MEMORY;
    server->listen_fd = -1;
    server->wake_read = -1;
    server->wake_write = -1;
    server->shutdown_read = -1;
    server->shutdown_write = -1;
    if (pthread_mutex_init(&server->mutex, NULL) != 0) {
        free(server);
        return MAELYS_MCP_ERR_MEMORY;
    }
    server->adapter = options->adapter;
    server->authenticator = options->authenticator;
    server->max_body_bytes = options->max_body_bytes
        ? options->max_body_bytes : MAELYS_MCP_DEFAULT_MAX_MESSAGE_BYTES;
    server->max_connections = options->max_connections
        ? options->max_connections : MAELYS_HTTP_MAX_CONNECTIONS;
    server->header_timeout_ms = options->header_timeout_ms
        ? options->header_timeout_ms : MAELYS_HTTP_HEADER_TIMEOUT_MS;
    server->body_timeout_ms = options->body_timeout_ms
        ? options->body_timeout_ms : MAELYS_HTTP_BODY_TIMEOUT_MS;
    server->idle_timeout_ms = options->idle_timeout_ms
        ? options->idle_timeout_ms : MAELYS_HTTP_IDLE_TIMEOUT_MS;
    server->write_timeout_ms = options->write_timeout_ms
        ? options->write_timeout_ms : MAELYS_MCP_DEFAULT_STDIO_WRITE_TIMEOUT_MS;
    server->endpoint_path = strdup(options->endpoint_path
        ? options->endpoint_path : DEFAULT_ENDPOINT_PATH);
    if (!server->endpoint_path) {
        server_free(server);
        return MAELYS_MCP_ERR_MEMORY;
    }
    if (options->allowed_origin_count) {
        server->allowed_origins = calloc(options->allowed_origin_count,
            sizeof(*server->allowed_origins));
        if (!server->allowed_origins) {
            server_free(server);
            return MAELYS_MCP_ERR_MEMORY;
        }
        for (size_t index = 0; index < options->allowed_origin_count; ++index) {
            server->allowed_origins[index] = strdup(options->allowed_origins[index]);
            if (!server->allowed_origins[index]) {
                server->allowed_origin_count = index;
                server_free(server);
                return MAELYS_MCP_ERR_MEMORY;
            }
        }
        server->allowed_origin_count = options->allowed_origin_count;
    }
    struct sockaddr_storage resolved;
    socklen_t resolved_length = 0;
    int family = 0;
    maelys_mcp_result_t status = resolve_bind(
        options->bind_address ? options->bind_address : DEFAULT_BIND_ADDRESS,
        options->port, &resolved, &resolved_length, &family,
        &server->bind_is_loopback);
    if (status != MAELYS_MCP_OK) {
        server_free(server);
        return status;
    }
    /*
     * Binding any address other than loopback requires an authenticator other
     * than loopback-trust, and refuses to start without one.
     *
     * Before the socket exists, deliberately. The check is here rather than in
     * the CLI because it is a property of the listener and a second entry point
     * must not be able to skip it; it is before bind_listener because a check
     * that bound first would have published the port for as long as it took to
     * refuse.
     */
    if (!server->bind_is_loopback &&
        maelys_http_auth_requires_loopback(options->authenticator)) {
        server_free(server);
        return MAELYS_MCP_ERR_DENIED;
    }
    status = bind_listener(server, &resolved, resolved_length, family);
    if (status != MAELYS_MCP_OK) {
        server_free(server);
        return status;
    }
    if (create_wakeup_pipe(&server->wake_read, &server->wake_write) != 0) {
        server_free(server);
        return MAELYS_MCP_ERR_IO;
    }
    /* Created before the acceptor exists, so every connection thread that can
     * ever run already has a valid descriptor to hand the adapter. */
    if (create_wakeup_pipe(&server->shutdown_read, &server->shutdown_write) != 0) {
        server_free(server);
        return MAELYS_MCP_ERR_IO;
    }
    if (pthread_create(&server->acceptor, NULL, acceptor_main, server) != 0) {
        server_free(server);
        return MAELYS_MCP_ERR_IO;
    }
    server->acceptor_started = 1;
    *out_server = server;
    return MAELYS_MCP_OK;
}

unsigned short maelys_http_server_port(const maelys_http_server_t *server) {
    return server ? server->port : 0u;
}

/*
 * How long phase 2 gives every in-flight exchange to end gracefully before
 * phase 3 stops being polite. It is the adapter's own close deadline plus a
 * margin for the writes that follow it: a shorter value would cut the streams
 * phase 2 exists to let finish, and a longer one would only delay a shutdown
 * that the adapter has already bounded on its own.
 */
#define MAELYS_HTTP_DRAIN_GRACE_MS (MAELYS_MCP_HTTP_CLOSE_TIMEOUT_MS + 1000u)

maelys_mcp_result_t maelys_http_server_stop(maelys_http_server_t *server) {
    if (!server) return MAELYS_MCP_ERR_ARGUMENT;
    /*
     * Four ordered phases, and the order is the whole content.
     *
     * 1. Stop accepting. In-flight requests are untouched, so no client loses
     *    an answer because a later one arrived.
     * 2. Ask every open stream to FINISH, which is not the same as stopping it:
     *    each surviving subscription is completed with `resultType: "complete"`,
     *    those frames are written, and the body is terminated properly. This is
     *    the phase that would not exist if shutdown were spelled as
     *    cancellation, and the difference is visible on the wire.
     * 3. Only then stop being polite: shutdown() the stragglers, which is what
     *    makes a connection parked in a header read leave promptly.
     * 4. Join.
     *
     * Reversing 1 and 2 would race a connection that arrived after the drain
     * started; reversing 2 and 3 would truncate exactly the streams 2 exists to
     * let finish.
     */
    pthread_mutex_lock(&server->mutex);
    server->stopping = 1;
    pthread_mutex_unlock(&server->mutex);
    if (server->wake_write >= 0) {
        ssize_t written = write(server->wake_write, "x", 1u);
        (void)written;
    }
    if (server->acceptor_started) pthread_join(server->acceptor, NULL);
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }

    /* Phase 2. One byte, read by nobody and polled by every adapter that is
     * currently draining. */
    if (server->shutdown_write >= 0) {
        ssize_t written = write(server->shutdown_write, "x", 1u);
        (void)written;
    }
    uint64_t grace = deadline_from(MAELYS_HTTP_DRAIN_GRACE_MS);
    for (;;) {
        pthread_mutex_lock(&server->mutex);
        size_t in_flight = server->exchange_count;
        pthread_mutex_unlock(&server->mutex);
        if (!in_flight) break;
        uint64_t now = 0u;
        if (monotonic_milliseconds(&now) != 0 || now >= grace) break;
        /*
         * A 2 ms sleep and not a condition variable, because this is the only
         * place in the process that waits on this count and it runs once per
         * lifetime. A condvar here would add a signal to the exchange hot path
         * to save a handful of wakeups on a path taken at exit.
         */
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 2 * 1000 * 1000};
        (void)nanosleep(&pause, NULL);
    }

    /*
     * Phase 3. Every connection thread terminates in bounded time on its own -
     * each read carries a deadline, each write carries the write timeout, and
     * each exchange's close deadline detaches rather than waits - so this is
     * what makes it prompt rather than what makes it terminate.
     */
    pthread_mutex_lock(&server->mutex);
    for (connection_t *entry = server->connections; entry; entry = entry->next) {
        if (!entry->finished) (void)shutdown(entry->fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&server->mutex);
    reap_connections(server, 1);
    server_free(server);
    return MAELYS_MCP_OK;
}
