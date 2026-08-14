#include "src/internal/internal.h"
#include "tests/test_support.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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

int main(void) {
    static const maelys_test_case_t tests[] = {
        {"stdout isolation argument contract", test_argument_contract},
        {"library stdout cannot contaminate MCP transport", test_library_stdout_cannot_contaminate_protocol}
    };
    return maelys_run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
