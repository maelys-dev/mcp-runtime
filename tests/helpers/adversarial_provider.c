#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MAELYS_TEST_ADDRESS_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define MAELYS_TEST_ADDRESS_SANITIZER 1
#endif

static int describe_and_shutdown(const char *name, int stubborn) {
    char request[4096];
    while (fgets(request, sizeof(request), stdin)) {
        if (strstr(request, "provider/describe")) {
            printf("{\"protocol\":\"maelys-provider/2\",\"id\":1,\"result\":{"
                "\"name\":\"%s\",\"version\":\"1\",\"tools\":[]}}\n", name);
            if (fflush(stdout) != 0) return 3;
        } else if (strstr(request, "provider/shutdown")) {
            if (stubborn) {
                (void)signal(SIGTERM, SIG_IGN);
                for (;;) pause();
            }
            puts("{\"protocol\":\"maelys-provider/2\",\"id\":2,\"result\":{}}");
            return fflush(stdout) == 0 ? 0 : 3;
        } else {
            return 5;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    char request[4096];
    if (strstr(argv[0], "fd-check")) {
#ifndef MAELYS_TEST_ADDRESS_SANITIZER
        for (int fd = 3; fd < 256; ++fd) {
            int type = 0;
            socklen_t length = sizeof(type);
            if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) == 0) return 6;
        }
#endif
        return describe_and_shutdown("fd-check", 0);
    }
    if (strstr(argv[0], "stubborn")) {
        return describe_and_shutdown("stubborn", 1);
    }
    if (strstr(argv[0], "slow-describe")) {
        if (!fgets(request, sizeof(request), stdin)) return 1;
        struct timespec delay = {.tv_sec = 1, .tv_nsec = 0};
        while (nanosleep(&delay, &delay) != 0) {}
        puts("{\"protocol\":\"maelys-provider/2\",\"id\":1,\"result\":{"
            "\"name\":\"slow\",\"version\":\"1\",\"tools\":[]}}");
        return fflush(stdout) == 0 ? 0 : 3;
    }
    if (strstr(argv[0], "environment")) {
        const char *path = getenv("PATH");
#ifdef __APPLE__
        if (!path || strcmp(path, "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin") != 0) return 4;
#else
        if (!path || strcmp(path, "/usr/local/bin:/usr/bin:/bin") != 0) return 4;
#endif
        while (fgets(request, sizeof(request), stdin)) {
            if (strstr(request, "provider/describe")) {
                puts("{\"protocol\":\"maelys-provider/2\",\"id\":1,\"result\":{"
                    "\"name\":\"environment\",\"version\":\"1\",\"tools\":[]}}");
            } else if (strstr(request, "provider/shutdown")) {
                puts("{\"protocol\":\"maelys-provider/2\",\"id\":2,\"result\":{}}");
                return fflush(stdout) == 0 ? 0 : 3;
            } else {
                return 5;
            }
            if (fflush(stdout) != 0) return 3;
        }
        return 0;
    }
    if (!fgets(request, sizeof(request), stdin)) return 1;
    if (strstr(argv[0], "bad-json")) {
        puts("not-json");
    } else if (strstr(argv[0], "bad-envelope")) {
        puts("{\"protocol\":\"maelys-provider/2\",\"id\":999,\"result\":{}}");
    } else if (strstr(argv[0], "bad-schema")) {
        puts("{\"protocol\":\"maelys-provider/2\",\"id\":1,\"result\":{"
            "\"name\":\"bad-schema\",\"version\":\"1\",\"tools\":[{"
            "\"name\":\"bad.tool\",\"description\":\"unsupported schema\","
            "\"inputSchema\":{\"type\":\"object\",\"oneOf\":[]},\"effect\":\"read\"}]}}");
    } else if (strstr(argv[0], "oversized")) {
        for (int index = 0; index < 4096; ++index) putchar('x');
        putchar('\n');
    } else {
        return 2;
    }
    return fflush(stdout) == 0 ? 0 : 3;
}
