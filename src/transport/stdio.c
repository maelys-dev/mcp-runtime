#include "src/internal/internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct stdio_writer_context {
    maelys_mcp_channel_t *channel;
    int write_fd;
    int wake_fd;
    unsigned int write_timeout_ms;
    atomic_int status;
} stdio_writer_context_t;

static void signal_writer_failure(
    stdio_writer_context_t *context,
    maelys_mcp_result_t status) {
    atomic_store(&context->status, (int)status);
    const unsigned char signal = 1u;
    ssize_t ignored = write(context->wake_fd, &signal, sizeof(signal));
    (void)ignored;
}

static void *stdio_writer_main(void *opaque) {
    stdio_writer_context_t *context = opaque;
    for (;;) {
        json_t *message = NULL;
        maelys_mcp_result_t status = maelys_mcp_channel_next(
            context->channel, 1000u, &message);
        if (status == MAELYS_MCP_ERR_TIMEOUT) continue;
        if (status == MAELYS_MCP_ERR_CLOSED) {
            /*
             * The outbox closed under us. On the graceful path the reader has
             * already left its loop and nobody is listening; on the path that
             * matters - an offloaded call faulted the channel from its own
             * thread - the reader is blocked in poll() and would otherwise sit
             * there with no writer. Waking it costs nothing in the first case
             * and is the only signal in the second.
             */
            signal_writer_failure(context, MAELYS_MCP_OK);
            return NULL;
        }
        if (status != MAELYS_MCP_OK) {
            signal_writer_failure(context, status);
            return NULL;
        }
        status = maelys_mcp_write_json_line_with_timeout(
            context->write_fd, message, context->write_timeout_ms);
        json_decref(message);
        if (status != MAELYS_MCP_OK) {
            signal_writer_failure(context, status);
            return NULL;
        }
    }
}

static int set_descriptor_flag(int fd, int command, int flag) {
    int current = fcntl(fd, command);
    if (current < 0) return -1;
    int set_command = command == F_GETFD ? F_SETFD : F_SETFL;
    return fcntl(fd, set_command, current | flag);
}

static maelys_mcp_result_t create_wakeup_pipe(int descriptors[2]) {
    descriptors[0] = -1;
    descriptors[1] = -1;
    if (pipe(descriptors) != 0 ||
        set_descriptor_flag(descriptors[0], F_GETFD, FD_CLOEXEC) != 0 ||
        set_descriptor_flag(descriptors[1], F_GETFD, FD_CLOEXEC) != 0 ||
        set_descriptor_flag(descriptors[1], F_GETFL, O_NONBLOCK) != 0) {
        if (descriptors[0] >= 0) close(descriptors[0]);
        if (descriptors[1] >= 0) close(descriptors[1]);
        descriptors[0] = -1;
        descriptors[1] = -1;
        return MAELYS_MCP_ERR_IO;
    }
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t wait_for_input_or_writer(
    int read_fd,
    int wake_fd,
    int *out_writer_failed) {
    struct pollfd descriptors[2] = {
        {.fd = read_fd, .events = POLLIN},
        {.fd = wake_fd, .events = POLLIN}
    };
    *out_writer_failed = 0;
    for (;;) {
        int ready = poll(descriptors, 2u, -1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return MAELYS_MCP_ERR_IO;
        if (descriptors[1].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) {
            *out_writer_failed = 1;
            return MAELYS_MCP_OK;
        }
        if (descriptors[0].revents & (POLLIN | POLLHUP)) return MAELYS_MCP_OK;
        if (descriptors[0].revents & (POLLERR | POLLNVAL)) return MAELYS_MCP_ERR_IO;
    }
}

maelys_mcp_result_t maelys_mcp_stdio_finish_status(
    maelys_mcp_result_t primary_status,
    maelys_mcp_result_t close_status,
    maelys_mcp_result_t writer_status,
    maelys_mcp_result_t destroy_status,
    maelys_mcp_result_t flags_status) {
    if (primary_status != MAELYS_MCP_OK) return primary_status;
    if (close_status != MAELYS_MCP_OK) return close_status;
    if (writer_status != MAELYS_MCP_OK) return writer_status;
    if (destroy_status != MAELYS_MCP_OK) return destroy_status;
    return flags_status;
}

static maelys_mcp_result_t finish_stdio(
    maelys_mcp_line_reader_t *reader,
    maelys_mcp_channel_t *channel,
    stdio_writer_context_t *writer_context,
    pthread_t writer,
    int writer_started,
    int wake_pipe[2],
    int write_fd,
    int write_flags,
    maelys_mcp_result_t status,
    int drain) {
    maelys_mcp_line_reader_clear(reader);
    maelys_mcp_result_t close_status = MAELYS_MCP_OK;
    if (drain) {
        close_status = maelys_mcp_channel_close(channel, 0u);
        if (close_status != MAELYS_MCP_OK) maelys_mcp_channel_abort(channel);
    } else {
        maelys_mcp_channel_abort(channel);
    }
    if (writer_started) pthread_join(writer, NULL);
    maelys_mcp_result_t writer_status = writer_started ?
        (maelys_mcp_result_t)atomic_load(&writer_context->status) : MAELYS_MCP_OK;
    maelys_mcp_result_t destroy_status = maelys_mcp_channel_destroy(channel);
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    maelys_mcp_result_t flags_status = fcntl(write_fd, F_SETFL, write_flags) == 0 ?
        MAELYS_MCP_OK : MAELYS_MCP_ERR_IO;
    return maelys_mcp_stdio_finish_status(status, close_status, writer_status,
        destroy_status, flags_status);
}

maelys_mcp_result_t maelys_mcp_runtime_serve_stdio_with_options(
    maelys_mcp_runtime_t *runtime,
    int read_fd,
    int write_fd,
    const maelys_mcp_stdio_options_t *options) {
    if (!runtime || read_fd < 0 || write_fd < 0) return MAELYS_MCP_ERR_ARGUMENT;
    unsigned int write_timeout_ms = options && options->write_timeout_ms ?
        options->write_timeout_ms : MAELYS_MCP_DEFAULT_STDIO_WRITE_TIMEOUT_MS;
    maelys_mcp_line_reader_t reader;
    maelys_mcp_result_t initialized = maelys_mcp_line_reader_init(
        &reader, runtime->max_message_bytes);
    if (initialized != MAELYS_MCP_OK) return initialized;
    int wake_pipe[2];
    initialized = create_wakeup_pipe(wake_pipe);
    if (initialized != MAELYS_MCP_OK) {
        maelys_mcp_line_reader_clear(&reader);
        return initialized;
    }
    int write_flags = fcntl(write_fd, F_GETFL);
    if (write_flags < 0 || fcntl(write_fd, F_SETFL, write_flags | O_NONBLOCK) != 0) {
        maelys_mcp_line_reader_clear(&reader);
        close(wake_pipe[0]);
        close(wake_pipe[1]);
        return MAELYS_MCP_ERR_IO;
    }
    maelys_mcp_channel_config_t default_channel_config = {
        .max_messages = 256u,
        .max_bytes = runtime->max_message_bytes > SIZE_MAX / 4u ?
            SIZE_MAX : runtime->max_message_bytes * 4u,
        .response_burst = 8u,
        .admission_timeout_ms = write_timeout_ms,
        .close_timeout_ms = write_timeout_ms
    };
    const maelys_mcp_channel_config_t *channel_config = options && options->channel_config ?
        options->channel_config : &default_channel_config;
    maelys_mcp_channel_t *channel = NULL;
    initialized = maelys_mcp_channel_create(runtime, channel_config, &channel);
    if (initialized != MAELYS_MCP_OK) {
        maelys_mcp_line_reader_clear(&reader);
        close(wake_pipe[0]);
        close(wake_pipe[1]);
        (void)fcntl(write_fd, F_SETFL, write_flags);
        return initialized;
    }
    stdio_writer_context_t writer_context = {
        .channel = channel,
        .write_fd = write_fd,
        .wake_fd = wake_pipe[1],
        .write_timeout_ms = write_timeout_ms
    };
    atomic_init(&writer_context.status, (int)MAELYS_MCP_OK);
    pthread_t writer;
    memset(&writer, 0, sizeof(writer));
    int writer_started = 0;
    if (pthread_create(&writer, NULL, stdio_writer_main, &writer_context) != 0) {
        return finish_stdio(&reader, channel, &writer_context, writer,
            writer_started, wake_pipe, write_fd, write_flags,
            MAELYS_MCP_ERR_IO, 0);
    }
    writer_started = 1;
    for (;;) {
        json_t *request = NULL;
        char *error = NULL;
        maelys_mcp_result_t status = maelys_mcp_line_reader_next(
            &reader, &request, &error);
        int eof = 0;
        if (status == MAELYS_MCP_ERR_NOT_FOUND) {
            int writer_failed = 0;
            status = wait_for_input_or_writer(read_fd, wake_pipe[0], &writer_failed);
            if (status != MAELYS_MCP_OK) {
                free(error);
                return finish_stdio(&reader, channel, &writer_context, writer,
                    writer_started, wake_pipe, write_fd, write_flags, status, 0);
            }
            if (writer_failed) {
                free(error);
                status = (maelys_mcp_result_t)atomic_load(&writer_context.status);
                /* An offloaded dispatch that failed reports through the
                 * channel, not through the writer: it had no way to reach this
                 * frame's caller, because there wasn't one. */
                if (status == MAELYS_MCP_OK) {
                    status = maelys_mcp_channel_dispatch_status(channel);
                }
                if (status == MAELYS_MCP_OK) status = MAELYS_MCP_ERR_IO;
                return finish_stdio(&reader, channel, &writer_context, writer,
                    writer_started, wake_pipe, write_fd, write_flags, status, 0);
            }
            status = maelys_mcp_line_reader_read_once(
                &reader, read_fd, &request, &error, &eof);
            if (status == MAELYS_MCP_ERR_NOT_FOUND && !eof) {
                free(error);
                continue;
            }
        }
        if (status == MAELYS_MCP_ERR_NOT_FOUND && eof) {
            free(error);
            return finish_stdio(&reader, channel, &writer_context, writer,
                writer_started, wake_pipe, write_fd, write_flags, MAELYS_MCP_OK, 1);
        }
        if (status != MAELYS_MCP_OK) {
            json_t *response = maelys_mcp_error_response(NULL, -32700,
                error ? error : "Parse error", NULL);
            free(error);
            if (!response) {
                return finish_stdio(&reader, channel, &writer_context, writer,
                    writer_started, wake_pipe, write_fd, write_flags,
                    MAELYS_MCP_ERR_MEMORY, 0);
            }
            status = maelys_mcp_channel_enqueue_take(channel, response,
                MAELYS_MCP_OUTBOX_RESPONSE, NULL, 1);
            if (status != MAELYS_MCP_OK) {
                json_decref(response);
                return finish_stdio(&reader, channel, &writer_context, writer,
                    writer_started, wake_pipe, write_fd, write_flags, status, 0);
            }
            continue;
        }
        free(error);
        /*
         * The demux, not the dispatcher. This is the whole of what stdio knows
         * about nested requests and concurrent calls: hand the frame to the
         * channel and go back to reading. Whether it was a reply this
         * connection was waiting on, a call that belongs on its own thread, or
         * an ordinary inline request is the channel's business - which is what
         * makes this loop reusable by a transport that is not stdio.
         */
        status = maelys_mcp_channel_accept(channel, request);
        json_decref(request);
        if (status == MAELYS_MCP_OK) {
            status = maelys_mcp_channel_dispatch_status(channel);
        }
        if (status != MAELYS_MCP_OK) {
            return finish_stdio(&reader, channel, &writer_context, writer,
                writer_started, wake_pipe, write_fd, write_flags, status, 0);
        }
    }
}

maelys_mcp_result_t maelys_mcp_runtime_serve_stdio(
    maelys_mcp_runtime_t *runtime,
    int read_fd,
    int write_fd) {
    return maelys_mcp_runtime_serve_stdio_with_options(
        runtime, read_fd, write_fd, NULL);
}
