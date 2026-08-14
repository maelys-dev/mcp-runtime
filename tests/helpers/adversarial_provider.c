#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    (void)argc;
    char request[4096];
    if (strstr(argv[0], "environment")) {
        const char *path = getenv("PATH");
        if (!path || strcmp(path, "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin") != 0) return 4;
        while (fgets(request, sizeof(request), stdin)) {
            if (strstr(request, "provider/describe")) {
                puts("{\"protocol\":\"maelys-provider/1\",\"id\":1,\"result\":{"
                    "\"name\":\"environment\",\"version\":\"1\",\"tools\":[]}}");
            } else if (strstr(request, "provider/shutdown")) {
                puts("{\"protocol\":\"maelys-provider/1\",\"id\":2,\"result\":{}}");
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
        puts("{\"protocol\":\"maelys-provider/1\",\"id\":999,\"result\":{}}");
    } else if (strstr(argv[0], "bad-schema")) {
        puts("{\"protocol\":\"maelys-provider/1\",\"id\":1,\"result\":{"
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
