#include "src/internal/internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

maelys_mcp_result_t maelys_mcp_isolate_stdout(int *out_transport_fd) {
    if (!out_transport_fd) return MAELYS_MCP_ERR_ARGUMENT;
    *out_transport_fd = -1;
    if (fflush(stdout) != 0) return MAELYS_MCP_ERR_IO;

    int transport_fd;
#ifdef F_DUPFD_CLOEXEC
    transport_fd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
#else
    transport_fd = dup(STDOUT_FILENO);
    if (transport_fd >= 0 && fcntl(transport_fd, F_SETFD, FD_CLOEXEC) < 0) {
        int saved = errno;
        close(transport_fd);
        errno = saved;
        transport_fd = -1;
    }
#endif
    if (transport_fd < 0) return MAELYS_MCP_ERR_IO;
    if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        int saved = errno;
        close(transport_fd);
        errno = saved;
        return MAELYS_MCP_ERR_IO;
    }
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    *out_transport_fd = transport_fd;
    return MAELYS_MCP_OK;
}
