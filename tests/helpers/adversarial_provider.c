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
#if __has_feature(thread_sanitizer)
#define MAELYS_TEST_THREAD_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define MAELYS_TEST_ADDRESS_SANITIZER 1
#endif
#if defined(__SANITIZE_THREAD__)
#define MAELYS_TEST_THREAD_SANITIZER 1
#endif

static int describe_and_shutdown(const char *name, int stubborn) {
    char request[4096];
    while (fgets(request, sizeof(request), stdin)) {
        if (strstr(request, "provider/describe")) {
            printf("{\"protocol\":\"maelys-provider/3\",\"id\":1,\"result\":{"
                "\"name\":\"%s\",\"version\":\"1\",\"tools\":[]}}\n", name);
            if (fflush(stdout) != 0) return 3;
        } else if (strstr(request, "provider/shutdown")) {
            if (stubborn) {
                (void)signal(SIGTERM, SIG_IGN);
                for (;;) pause();
            }
            puts("{\"protocol\":\"maelys-provider/3\",\"id\":2,\"result\":{}}");
            return fflush(stdout) == 0 ? 0 : 3;
        } else {
            return 5;
        }
    }
    return 0;
}

/*
 * The modes below speak real MCP rather than the maelys-provider wire: they
 * are upstreams for the stdio MCP proxy provider (docs/mcp-proxy.md). The
 * dogfood rig for that provider is mcp-runtime proxying mcp-runtime, so these
 * exist only for what the real host binary cannot be made to do - refuse
 * server/discover, fail a call, open a server-to-client request, or die
 * mid-call.
 */
typedef enum mcp_upstream_mode {
    MCP_UPSTREAM_LEGACY,
    MCP_UPSTREAM_ERRORING,
    MCP_UPSTREAM_CHATTY,
    MCP_UPSTREAM_DYING,
    /* One valid tool alongside one whose inputSchema
     * maelys_mcp_validate_schema_definition rejects (an empty oneOf) - the
     * upstream for the proxy's schema_policy tests (docs/mcp-proxy.md). */
    MCP_UPSTREAM_EXOTIC_SCHEMA
} mcp_upstream_mode_t;

/* The proxy writes jsonrpc, id, method, params in that order and never nests
 * an "id" in params, so the first occurrence is the request id. */
static long request_id(const char *request) {
    const char *marker = strstr(request, "\"id\":");
    return marker ? strtol(marker + 5, NULL, 10) : 0;
}

/* Whitebox by design, like request_id above: matches the tool name a
 * tools/call request names, without parsing the request as JSON. */
static int request_names_tool(const char *request, const char *name) {
    char needle[128];
    (void)snprintf(needle, sizeof(needle), "\"name\":\"%s\"", name);
    return strstr(request, needle) != NULL;
}

static int mcp_upstream(mcp_upstream_mode_t mode) {
    char request[8192];
    const char *tool = mode == MCP_UPSTREAM_LEGACY ? "legacy.echo" : "proxy.probe";
    while (fgets(request, sizeof(request), stdin)) {
        long id = request_id(request);
        if (strstr(request, "\"notifications/initialized\"")) continue;
        if (strstr(request, "\"server/discover\"")) {
            if (mode == MCP_UPSTREAM_LEGACY) {
                printf("{\"jsonrpc\":\"2.0\",\"id\":%ld,\"error\":{\"code\":-32601,"
                    "\"message\":\"Method not found\"}}\n", id);
            } else {
                printf("{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"supportedVersions\":"
                    "[\"2026-07-28\",\"2025-11-25\"],\"capabilities\":{\"tools\":{}}}}\n", id);
            }
        } else if (strstr(request, "\"initialize\"")) {
            printf("{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"protocolVersion\":"
                "\"2025-11-25\",\"capabilities\":{\"tools\":{}},\"serverInfo\":"
                "{\"name\":\"legacy-upstream\",\"version\":\"1\"}}}\n", id);
        } else if (strstr(request, "\"tools/list\"")) {
            if (mode == MCP_UPSTREAM_EXOTIC_SCHEMA) {
                printf("{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"tools\":["
                    "{\"name\":\"proxy.good\",\"title\":\"Good tool\","
                    "\"description\":\"A fixture tool with a valid schema.\","
                    "\"inputSchema\":{\"type\":\"object\"}},"
                    "{\"name\":\"proxy.exotic\",\"title\":\"Exotic tool\","
                    "\"description\":\"A fixture tool with an unsupported schema.\","
                    "\"inputSchema\":{\"type\":\"object\",\"oneOf\":[]}}"
                    "]}}\n", id);
            } else {
                printf("{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"tools\":[{\"name\":\"%s\","
                    "\"title\":\"Fixture tool\",\"description\":\"A fixture upstream tool.\","
                    "\"inputSchema\":{\"type\":\"object\"}}]}}\n", id, tool);
            }
        } else if (strstr(request, "\"tools/call\"")) {
            if (mode == MCP_UPSTREAM_DYING) {
                (void)fflush(stdout);
                _exit(0);
            }
            if (mode == MCP_UPSTREAM_EXOTIC_SCHEMA) {
                /* proxy.exotic is only ever called under
                 * MAELYS_MCP_PROXY_SCHEMA_PASSTHROUGH, which forwards
                 * arguments without local validation - this upstream is the
                 * one that rejects them, standing in for real argument
                 * validation an upstream would do itself. */
                if (request_names_tool(request, "proxy.exotic")) {
                    printf("{\"jsonrpc\":\"2.0\",\"id\":%ld,\"error\":{\"code\":-32000,"
                        "\"message\":\"exotic tool arguments rejected by upstream\"}}\n", id);
                } else {
                    printf("{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"content\":"
                        "[{\"type\":\"text\",\"text\":\"good-ok\"}]}}\n", id);
                }
            } else if (mode == MCP_UPSTREAM_ERRORING) {
                printf("{\"jsonrpc\":\"2.0\",\"id\":%ld,\"error\":{\"code\":-32000,"
                    "\"message\":\"upstream refused the call\"}}\n", id);
            } else if (mode == MCP_UPSTREAM_CHATTY) {
                /* An id-less notification the proxy does not handle, then a
                 * genuine server-to-client request it must refuse without
                 * faulting the transport. */
                printf("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/message\","
                    "\"params\":{\"level\":\"info\",\"data\":\"noise\"}}\n");
                printf("{\"jsonrpc\":\"2.0\",\"id\":9001,\"method\":"
                    "\"sampling/createMessage\",\"params\":{}}\n");
                if (fflush(stdout) != 0) return 3;
                char reply[8192];
                const char *text = fgets(reply, sizeof(reply), stdin) &&
                    strstr(reply, "-32601") && strstr(reply, "9001") ?
                    "chatty-ok" : "chatty-unrefused";
                printf("{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"content\":"
                    "[{\"type\":\"text\",\"text\":\"%s\"}]}}\n", id, text);
            } else {
                printf("{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"content\":"
                    "[{\"type\":\"text\",\"text\":\"legacy-ok\"}]}}\n", id);
            }
        } else {
            printf("{\"jsonrpc\":\"2.0\",\"id\":%ld,\"error\":{\"code\":-32601,"
                "\"message\":\"Method not found\"}}\n", id);
        }
        if (fflush(stdout) != 0) return 3;
    }
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    char request[4096];
    if (strstr(argv[0], "legacy-mcp-upstream")) return mcp_upstream(MCP_UPSTREAM_LEGACY);
    if (strstr(argv[0], "erroring-mcp-upstream")) return mcp_upstream(MCP_UPSTREAM_ERRORING);
    if (strstr(argv[0], "chatty-mcp-upstream")) return mcp_upstream(MCP_UPSTREAM_CHATTY);
    if (strstr(argv[0], "dying-mcp-upstream")) return mcp_upstream(MCP_UPSTREAM_DYING);
    if (strstr(argv[0], "exotic-schema-mcp-upstream")) return mcp_upstream(MCP_UPSTREAM_EXOTIC_SCHEMA);
    if (strstr(argv[0], "fd-check")) {
#if !defined(MAELYS_TEST_ADDRESS_SANITIZER) && \
    !defined(MAELYS_TEST_THREAD_SANITIZER)
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
        puts("{\"protocol\":\"maelys-provider/3\",\"id\":1,\"result\":{"
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
                puts("{\"protocol\":\"maelys-provider/3\",\"id\":1,\"result\":{"
                    "\"name\":\"environment\",\"version\":\"1\",\"tools\":[]}}");
            } else if (strstr(request, "provider/shutdown")) {
                puts("{\"protocol\":\"maelys-provider/3\",\"id\":2,\"result\":{}}");
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
        puts("{\"protocol\":\"maelys-provider/3\",\"id\":999,\"result\":{}}");
    } else if (strstr(argv[0], "bad-schema")) {
        puts("{\"protocol\":\"maelys-provider/3\",\"id\":1,\"result\":{"
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
