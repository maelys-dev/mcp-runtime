#include "src/internal/internal.h"
#include "tests/test_support.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char *read_all(int fd) {
    size_t length = 0;
    size_t capacity = 128;
    char *bytes = malloc(capacity);
    if (!bytes) return NULL;
    for (;;) {
        if (length + 1u == capacity) {
            capacity *= 2u;
            char *grown = realloc(bytes, capacity);
            if (!grown) { free(bytes); return NULL; }
            bytes = grown;
        }
        ssize_t count = read(fd, bytes + length, capacity - length - 1u);
        if (count == 0) break;
        if (count < 0) { free(bytes); return NULL; }
        length += (size_t)count;
    }
    bytes[length] = '\0';
    return bytes;
}

static int test_argument_contract(void) {
    ASSERT_TRUE(maelys_mcp_isolate_stdout(NULL) == MAELYS_MCP_ERR_ARGUMENT);
    return 0;
}

static int test_library_stdout_cannot_contaminate_protocol(void) {
    int protocol[2];
    int diagnostics[2];
    ASSERT_TRUE(pipe(protocol) == 0);
    ASSERT_TRUE(pipe(diagnostics) == 0);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        close(protocol[0]);
        close(diagnostics[0]);
        if (dup2(protocol[1], STDOUT_FILENO) < 0 ||
            dup2(diagnostics[1], STDERR_FILENO) < 0) _exit(10);
        close(protocol[1]);
        close(diagnostics[1]);
        int transport_fd = -1;
        if (maelys_mcp_isolate_stdout(&transport_fd) != MAELYS_MCP_OK) _exit(11);
        if ((fcntl(transport_fd, F_GETFD) & FD_CLOEXEC) == 0) _exit(12);
        printf("third-party before call\n");
        const char response[] = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n";
        if (write(transport_fd, response, sizeof(response) - 1u) !=
            (ssize_t)(sizeof(response) - 1u)) _exit(13);
        printf("third-party during call\n");
        fprintf(stderr, "runtime diagnostic\n");
        close(transport_fd);
        _exit(0);
    }
    close(protocol[1]);
    close(diagnostics[1]);
    char *protocol_bytes = read_all(protocol[0]);
    char *diagnostic_bytes = read_all(diagnostics[0]);
    close(protocol[0]);
    close(diagnostics[0]);
    int status = 0;
    ASSERT_TRUE(waitpid(pid, &status, 0) == pid);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    ASSERT_TRUE(protocol_bytes && strcmp(protocol_bytes,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n") == 0);
    ASSERT_TRUE(diagnostic_bytes && strstr(diagnostic_bytes, "third-party before call"));
    ASSERT_TRUE(strstr(diagnostic_bytes, "third-party during call"));
    ASSERT_TRUE(strstr(diagnostic_bytes, "runtime diagnostic"));
    free(protocol_bytes);
    free(diagnostic_bytes);
    return 0;
}

static int fill_pipe(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return -1;
    char bytes[4096];
    memset(bytes, 'x', sizeof(bytes));
    for (;;) {
        ssize_t count = write(fd, bytes, sizeof(bytes));
        if (count > 0) continue;
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        (void)fcntl(fd, F_SETFL, flags);
        return -1;
    }
    return fcntl(fd, F_SETFL, flags);
}

static int wait_for_child(pid_t pid, int *out_status, unsigned int timeout_ms) {
    const struct timespec interval = {.tv_sec = 0, .tv_nsec = 10000000L};
    unsigned int waited_ms = 0u;
    while (waited_ms < timeout_ms) {
        pid_t waited = waitpid(pid, out_status, WNOHANG);
        if (waited == pid) return 1;
        if (waited < 0) return 0;
        (void)nanosleep(&interval, NULL);
        waited_ms += 10u;
    }
    return 0;
}

static int test_stalled_client_fails_within_write_deadline(void) {
    int input[2];
    int output[2];
    ASSERT_TRUE(pipe(input) == 0);
    ASSERT_TRUE(pipe(output) == 0);
    ASSERT_TRUE(fill_pipe(output[1]) == 0);
    ASSERT_TRUE(write(input[1], "{}\n", 3u) == 3);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        close(input[1]);
        close(output[0]);
        maelys_mcp_runtime_config_t config = {
            .server_name = "stdio-timeout",
            .server_version = "test"
        };
        maelys_mcp_runtime_t *runtime = NULL;
        if (maelys_mcp_runtime_create(&config, &runtime) != MAELYS_MCP_OK) _exit(20);
        maelys_mcp_stdio_options_t options = {.write_timeout_ms = 50u};
        maelys_mcp_result_t status = maelys_mcp_runtime_serve_stdio_with_options(
            runtime, input[0], output[1], &options);
        maelys_mcp_result_t destroy_status = maelys_mcp_runtime_destroy(runtime);
        if (status == MAELYS_MCP_OK) status = destroy_status;
        close(input[0]);
        close(output[1]);
        _exit(status == MAELYS_MCP_ERR_IO ? 0 : 21);
    }
    close(input[0]);
    close(output[1]);
    int status = 0;
    int exited = wait_for_child(pid, &status, 2000u);
    if (!exited) {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, &status, 0);
    }
    close(input[1]);
    close(output[0]);
    ASSERT_TRUE(exited);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 0;
}

static int test_failed_close_still_terminates_writer(void) {
    static const char request[] =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"subscriptions/listen\","
        "\"params\":{\"notifications\":{\"resourceSubscriptions\":[\"test://close\"]},"
        "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\","
        "\"io.modelcontextprotocol/clientInfo\":{\"name\":\"stdio-close\",\"version\":\"1\"},"
        "\"io.modelcontextprotocol/clientCapabilities\":{}}}}\n";
    int input[2];
    int output[2];
    ASSERT_TRUE(pipe(input) == 0);
    ASSERT_TRUE(pipe(output) == 0);
    ASSERT_TRUE(fill_pipe(output[1]) == 0);
    ASSERT_TRUE(write(input[1], request, sizeof(request) - 1u) ==
        (ssize_t)(sizeof(request) - 1u));
    ASSERT_TRUE(close(input[1]) == 0);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        close(output[0]);
        maelys_mcp_runtime_config_t config = {
            .server_name = "stdio-close-timeout",
            .server_version = "test"
        };
        maelys_mcp_runtime_t *runtime = NULL;
        if (maelys_mcp_runtime_create(&config, &runtime) != MAELYS_MCP_OK) _exit(30);
        if (maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_TOOLS) !=
                MAELYS_MCP_OK ||
            maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_RESOURCES) !=
                MAELYS_MCP_OK ||
            maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_SUBSCRIPTIONS) !=
                MAELYS_MCP_OK) _exit(31);
        maelys_mcp_stdio_options_t options = {.write_timeout_ms = 50u};
        maelys_mcp_result_t status = maelys_mcp_runtime_serve_stdio_with_options(
            runtime, input[0], output[1], &options);
        maelys_mcp_result_t destroy_status = maelys_mcp_runtime_destroy(runtime);
        close(input[0]);
        close(output[1]);
        if (destroy_status != MAELYS_MCP_OK) _exit(32);
        _exit(status == MAELYS_MCP_ERR_TIMEOUT ? 0 : 33);
    }
    close(input[0]);
    close(output[1]);
    int status = 0;
    int exited = wait_for_child(pid, &status, 2000u);
    if (!exited) {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, &status, 0);
    }
    close(output[0]);
    ASSERT_TRUE(exited);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 0;
}

static int test_stdio_restores_output_descriptor_flags(void) {
    int input[2];
    int output[2];
    ASSERT_TRUE(pipe(input) == 0);
    ASSERT_TRUE(pipe(output) == 0);
    int original_flags = fcntl(output[1], F_GETFL);
    ASSERT_TRUE(original_flags >= 0);
    ASSERT_TRUE(close(input[1]) == 0);
    maelys_mcp_runtime_config_t config = {
        .server_name = "stdio-flags",
        .server_version = "test"
    };
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(maelys_mcp_runtime_create(&config, &runtime) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_serve_stdio(runtime, input[0], output[1]) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(fcntl(output[1], F_GETFL) == original_flags);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    close(input[0]);
    close(output[0]);
    close(output[1]);
    return 0;
}

int main(void) {
    static const maelys_test_case_t tests[] = {
        {"stdout isolation argument contract", test_argument_contract},
        {"library stdout cannot contaminate MCP transport", test_library_stdout_cannot_contaminate_protocol},
        {"stalled stdio client fails within write deadline",
            test_stalled_client_fails_within_write_deadline},
        {"failed close still terminates stdio writer",
            test_failed_close_still_terminates_writer},
        {"stdio restores output descriptor flags",
            test_stdio_restores_output_descriptor_flags}
    };
    return maelys_run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
