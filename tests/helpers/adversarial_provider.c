#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    (void)argc;
    char request[4096];
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
