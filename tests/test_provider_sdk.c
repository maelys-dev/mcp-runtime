#include "maelys/mcp.h"

#include <jansson.h>

#include <pthread.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define ASSERT_TRUE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Assertion failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct sdk_fixture {
    int fd;
    pthread_t thread;
    json_t *input_schema;
    json_t *output_schema;
    json_t *resource_schema;
    maelys_mcp_provider_sdk_t *sdk;
    pthread_t producer_thread;
    int producer_started;
    int producer_joined;
    int producer_status;
    pthread_t late_producer_thread;
    int late_producer_started;
    int late_producer_joined;
    int late_producer_release;
    int late_producer_status;
    pthread_mutex_t producer_mutex;
    pthread_cond_t producer_release;
    int shutdown_calls;
} sdk_fixture_t;

static void set_error(char **out_error, const char *message) {
    if (!out_error) return;
    char *copy = strdup(message);
    if (!copy) return;
    free(*out_error);
    *out_error = copy;
}

static void *producer_main(void *opaque) {
    sdk_fixture_t *fixture = opaque;
    const maelys_mcp_provider_event_t event = {
        .kind = MAELYS_MCP_PROVIDER_EVENT_RESOURCES_LIST_CHANGED
    };
    fixture->producer_status = maelys_mcp_provider_sdk_emit_event(fixture->sdk, &event);
    return NULL;
}

static void *late_producer_main(void *opaque) {
    sdk_fixture_t *fixture = opaque;
    pthread_mutex_lock(&fixture->producer_mutex);
    while (!fixture->late_producer_release) {
        pthread_cond_wait(&fixture->producer_release, &fixture->producer_mutex);
    }
    pthread_mutex_unlock(&fixture->producer_mutex);
    const maelys_mcp_provider_event_t event = {
        .kind = MAELYS_MCP_PROVIDER_EVENT_TOOLS_LIST_CHANGED
    };
    fixture->late_producer_status = maelys_mcp_provider_sdk_emit_event(fixture->sdk, &event);
    return NULL;
}

static maelys_mcp_result_t call_tool(
    maelys_mcp_provider_sdk_t *sdk,
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    sdk_fixture_t *fixture = context;
    if (strcmp(request->tool_name, "sdk.echo") == 0) {
        json_t *message = json_object_get(request->arguments, "message");
        if (!json_is_string(message)) {
            set_error(out_error, "message required");
            return MAELYS_MCP_ERR_ARGUMENT;
        }
        out_result->type = MAELYS_MCP_PROVIDER_RESULT_COMPLETE;
        out_result->structured_content = json_pack("{s:s}",
            "message", json_string_value(message));
        return out_result->structured_content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
    }
    if (strcmp(request->tool_name, "sdk.events") == 0) {
        const maelys_mcp_provider_event_t updated = {
            .kind = MAELYS_MCP_PROVIDER_EVENT_RESOURCE_UPDATED,
            .resource_uri = "sdk://about"
        };
        const maelys_mcp_provider_event_t listed = {
            .kind = MAELYS_MCP_PROVIDER_EVENT_TOOLS_LIST_CHANGED
        };
        if (maelys_mcp_provider_sdk_emit_event(sdk, &updated) != MAELYS_MCP_OK ||
            maelys_mcp_provider_sdk_emit_event(sdk, &listed) != MAELYS_MCP_OK) {
            set_error(out_error, "event emission failed");
            return MAELYS_MCP_ERR_IO;
        }
        out_result->type = MAELYS_MCP_PROVIDER_RESULT_COMPLETE;
        out_result->structured_content = json_pack("{s:i}", "emitted", 2);
        return out_result->structured_content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
    }
    if (strcmp(request->tool_name, "sdk.async") == 0) {
        fixture->sdk = sdk;
        if (pthread_create(&fixture->producer_thread, NULL, producer_main, fixture) != 0) {
            set_error(out_error, "cannot start event producer");
            return MAELYS_MCP_ERR_IO;
        }
        fixture->producer_started = 1;
        out_result->type = MAELYS_MCP_PROVIDER_RESULT_COMPLETE;
        out_result->structured_content = json_pack("{s:b}", "started", 1);
        return out_result->structured_content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
    }
    if (strcmp(request->tool_name, "sdk.nested") == 0) {
        json_t *method = json_object_get(request->arguments, "method");
        json_t *inner = json_object_get(request->arguments, "params");
        json_t *client_result = NULL;
        char *nested_error = NULL;
        maelys_mcp_result_t nested_status = maelys_mcp_provider_sdk_request_client(
            sdk, json_is_string(method) ? json_string_value(method) : "elicitation/create",
            inner, &client_result, &nested_error);
        out_result->type = MAELYS_MCP_PROVIDER_RESULT_COMPLETE;
        out_result->structured_content = json_pack("{s:i,s:s,s:o}",
            "status", (int)nested_status,
            "error", nested_error ? nested_error : "",
            "result", client_result ? client_result : json_null());
        free(nested_error);
        return out_result->structured_content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
    }
    if (strcmp(request->tool_name, "sdk.shutdown-race") == 0) {
        fixture->sdk = sdk;
        if (pthread_create(&fixture->late_producer_thread, NULL, late_producer_main, fixture) != 0) {
            set_error(out_error, "cannot start late event producer");
            return MAELYS_MCP_ERR_IO;
        }
        fixture->late_producer_started = 1;
        out_result->type = MAELYS_MCP_PROVIDER_RESULT_COMPLETE;
        out_result->structured_content = json_pack("{s:b}", "started", 1);
        return out_result->structured_content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
    }
    set_error(out_error, "unknown tool");
    return MAELYS_MCP_ERR_NOT_FOUND;
}

static maelys_mcp_result_t read_resource(
    maelys_mcp_provider_sdk_t *sdk,
    void *context,
    const maelys_mcp_resource_request_t *request,
    maelys_mcp_resource_result_t *out_result,
    char **out_error) {
    (void)sdk;
    (void)context;
    if (strcmp(request->uri, "sdk://about") != 0) {
        set_error(out_error, "missing resource");
        return MAELYS_MCP_ERR_NOT_FOUND;
    }
    out_result->type = MAELYS_MCP_RESOURCE_RESULT_COMPLETE;
    out_result->contents = json_pack("[{s:s,s:s,s:s}]",
        "uri", request->uri, "mimeType", "text/plain", "text", "SDK resource");
    return out_result->contents ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static void destroy_context(void *context) {
    sdk_fixture_t *fixture = context;
    if (!fixture) return;
    if (fixture->input_schema) json_decref(fixture->input_schema);
    if (fixture->output_schema) json_decref(fixture->output_schema);
    if (fixture->resource_schema) json_decref(fixture->resource_schema);
}

static void shutdown_context(void *context) {
    sdk_fixture_t *fixture = context;
    if (!fixture) return;
    fixture->shutdown_calls++;
    if (fixture->producer_started && !fixture->producer_joined) {
        (void)pthread_join(fixture->producer_thread, NULL);
        fixture->producer_joined = 1;
    }
    if (fixture->late_producer_started && !fixture->late_producer_joined) {
        pthread_mutex_lock(&fixture->producer_mutex);
        fixture->late_producer_release = 1;
        pthread_cond_broadcast(&fixture->producer_release);
        pthread_mutex_unlock(&fixture->producer_mutex);
        (void)pthread_join(fixture->late_producer_thread, NULL);
        fixture->late_producer_joined = 1;
    }
}

static void *serve_main(void *opaque) {
    sdk_fixture_t *fixture = opaque;
    const maelys_mcp_tool_t tools[] = {
        {
            .name = "sdk.echo",
            .title = "Echo",
            .description = "Echoes a message.",
            .input_schema = fixture->input_schema,
            .output_schema = fixture->output_schema,
            .effect = MAELYS_MCP_EFFECT_READ
        },
        {
            .name = "sdk.events",
            .title = "Events",
            .description = "Emits provider events.",
            .input_schema = fixture->resource_schema,
            .effect = MAELYS_MCP_EFFECT_EXECUTE
        },
        {
            .name = "sdk.async",
            .title = "Async events",
            .description = "Emits an event from a provider worker thread.",
            .input_schema = fixture->resource_schema,
            .effect = MAELYS_MCP_EFFECT_EXECUTE
        },
        {
            .name = "sdk.shutdown-race",
            .title = "Shutdown race",
            .description = "Attempts an event after shutdown begins.",
            .input_schema = fixture->resource_schema,
            .effect = MAELYS_MCP_EFFECT_EXECUTE
        },
        {
            .name = "sdk.nested",
            .title = "Nested request",
            .description = "Opens a nested client request and reports what came back.",
            .input_schema = fixture->resource_schema,
            .effect = MAELYS_MCP_EFFECT_READ
        }
    };
    const maelys_mcp_resource_t resources[] = {
        {.uri = "sdk://about", .name = "About SDK", .mime_type = "text/plain"}
    };
    const maelys_mcp_provider_sdk_config_t config = {
        .name = "sdk-test",
        .version = "1.0",
        .tools = tools,
        .tool_count = sizeof(tools) / sizeof(tools[0]),
        .resources = resources,
        .resource_count = sizeof(resources) / sizeof(resources[0]),
        .call = call_tool,
        .read_resource = read_resource,
        .shutdown = shutdown_context,
        .destroy = destroy_context,
        .context = fixture
    };
    const maelys_mcp_provider_sdk_options_t options = {
        .max_message_bytes = 65536u,
        .input_fd = fixture->fd,
        .output_fd = fixture->fd
    };
    return (void *)(intptr_t)maelys_mcp_provider_sdk_serve(&config, &options);
}

static int write_json_line(int fd, json_t *message) {
    char *encoded = json_dumps(message, JSON_COMPACT | JSON_SORT_KEYS);
    if (!encoded) return 0;
    size_t length = strlen(encoded);
    int ok = write(fd, encoded, length) == (ssize_t)length &&
        write(fd, "\n", 1u) == 1;
    free(encoded);
    return ok;
}

static json_t *read_json_line(FILE *stream) {
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length = getline(&line, &capacity, stream);
    if (length < 0) {
        free(line);
        return NULL;
    }
    json_error_t error;
    json_t *value = json_loadb(line, (size_t)length, JSON_REJECT_DUPLICATES, &error);
    free(line);
    return value;
}

static int send_request(int fd, unsigned long id, const char *method, json_t *params) {
    json_t *request = json_pack("{s:s,s:i,s:s,s:o}",
        "protocol", MAELYS_MCP_PROVIDER_PROTOCOL,
        "id", (json_int_t)id,
        "method", method,
        "params", params ? params : json_object());
    if (!request) return 0;
    int ok = write_json_line(fd, request);
    json_decref(request);
    return ok;
}

static maelys_mcp_result_t activate_with_stdout(
    maelys_mcp_provider_sdk_t *sdk,
    void *context,
    char **out_error) {
    (void)sdk;
    (void)context;
    (void)out_error;
    fputs("application stdout must not reach the protocol\n", stdout);
    return fflush(stdout) == 0 ? MAELYS_MCP_OK : MAELYS_MCP_ERR_IO;
}

static int test_options_default_stdout_isolation(void) {
    int input[2] = {-1, -1};
    int output[2] = {-1, -1};
    ASSERT_TRUE(pipe(input) == 0);
    ASSERT_TRUE(pipe(output) == 0);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd < 0 || dup2(input[0], STDIN_FILENO) < 0 ||
            dup2(output[1], STDOUT_FILENO) < 0 || dup2(null_fd, STDERR_FILENO) < 0) {
            _exit(2);
        }
        close(input[0]);
        close(input[1]);
        close(output[0]);
        close(output[1]);
        close(null_fd);
        const maelys_mcp_provider_sdk_config_t config = {
            .name = "stdout-isolation",
            .version = "1.0",
            .activate = activate_with_stdout
        };
        const maelys_mcp_provider_sdk_options_t options = {
            .max_message_bytes = 65536u
        };
        _exit(maelys_mcp_provider_sdk_serve(&config, &options) == MAELYS_MCP_OK ? 0 : 3);
    }
    close(input[0]);
    close(output[1]);
    FILE *reader = fdopen(output[0], "r");
    ASSERT_TRUE(reader != NULL);
    ASSERT_TRUE(send_request(input[1], 1u, "provider/describe", NULL));
    ASSERT_TRUE(send_request(input[1], 2u, "provider/activate", NULL));
    ASSERT_TRUE(send_request(input[1], 3u, "provider/shutdown", NULL));
    close(input[1]);
    for (unsigned long id = 1u; id <= 3u; ++id) {
        json_t *response = read_json_line(reader);
        ASSERT_TRUE(json_is_object(response));
        ASSERT_TRUE(json_integer_value(json_object_get(response, "id")) == (json_int_t)id);
        ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
        json_decref(response);
    }
    fclose(reader);
    int child_status = 0;
    ASSERT_TRUE(waitpid(pid, &child_status, 0) == pid);
    ASSERT_TRUE(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    return 0;
}

static int test_invalid_catalog_descriptors(void) {
    const maelys_mcp_resource_t invalid_resource = {
        .uri = "not a URI", .name = "Invalid resource"
    };
    const maelys_mcp_provider_sdk_config_t invalid_resource_config = {
        .name = "invalid-catalog", .version = "1.0",
        .resources = &invalid_resource, .resource_count = 1u,
        .read_resource = read_resource
    };
    ASSERT_TRUE(maelys_mcp_provider_sdk_serve(&invalid_resource_config, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);
    const maelys_mcp_resource_template_t invalid_template = {
        .uri_template = "sdk://value/{item}"
    };
    const maelys_mcp_provider_sdk_config_t invalid_template_config = {
        .name = "invalid-template", .version = "1.0",
        .resource_templates = &invalid_template, .resource_template_count = 1u,
        .read_resource = read_resource
    };
    ASSERT_TRUE(maelys_mcp_provider_sdk_serve(&invalid_template_config, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);
    return 0;
}

/* ------------------------------------------------------- nested requests
 * maelys_mcp_provider_sdk_request_client, driven end to end over the same
 * socketpair the rest of this file uses: the SDK writes a real
 * provider/nestedRequest and these helpers play the host on the other end,
 * answering (or refusing, or misbehaving) on the same connection the call
 * arrived on - exactly the shape docs/provider-protocol.md describes.
 */

static json_t *nested_reply_success(const char *nested_id, const char *answer) {
    return json_pack("{s:s,s:s,s:{s:s,s:{s:s}}}",
        "protocol", MAELYS_MCP_PROVIDER_PROTOCOL,
        "method", "provider/nestedReply",
        "params", "nestedId", nested_id, "result", "answer", answer);
}

static json_t *nested_reply_error(const char *nested_id, const char *code, const char *message) {
    return json_pack("{s:s,s:s,s:{s:s,s:{s:s,s:s}}}",
        "protocol", MAELYS_MCP_PROVIDER_PROTOCOL,
        "method", "provider/nestedReply",
        "params", "nestedId", nested_id, "error", "code", code, "message", message);
}

/* Reads the provider/nestedRequest the SDK just wrote, checks its envelope
 * and requested method, and copies the nestedId the reply must echo. */
static int read_nested_request(FILE *reader, const char *expected_method, char *out_id, size_t out_size) {
    json_t *request = read_json_line(reader);
    ASSERT_TRUE(json_is_object(request));
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(request, "protocol")),
        MAELYS_MCP_PROVIDER_PROTOCOL) == 0);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(request, "method")),
        "provider/nestedRequest") == 0);
    json_t *params = json_object_get(request, "params");
    ASSERT_TRUE(json_is_object(params));
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(params, "method")), expected_method) == 0);
    json_t *id = json_object_get(params, "nestedId");
    ASSERT_TRUE(json_is_string(id));
    (void)snprintf(out_id, out_size, "%s", json_string_value(id));
    json_decref(request);
    return 0;
}

/* Happy path: the client answers, the helper returns the client's result
 * inside the tool's own response. */
static int nested_round_trip(int fd, FILE *reader, unsigned long id,
    const char *method, const char *prompt) {
    ASSERT_TRUE(send_request(fd, id, "provider/call",
        json_pack("{s:s,s:{s:s,s:{s:s}}}", "name", "sdk.nested", "arguments",
            "method", method, "params", "message", prompt)));
    char nested_id[64];
    ASSERT_TRUE(read_nested_request(reader, method, nested_id, sizeof(nested_id)) == 0);

    json_t *reply = nested_reply_success(nested_id, "yes");
    ASSERT_TRUE(reply != NULL);
    ASSERT_TRUE(write_json_line(fd, reply));
    json_decref(reply);

    json_t *response = read_json_line(reader);
    ASSERT_TRUE(json_is_object(response));
    json_t *structured = json_object_get(json_object_get(response, "result"), "structuredContent");
    ASSERT_TRUE(json_integer_value(json_object_get(structured, "status")) == (json_int_t)MAELYS_MCP_OK);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(
        json_object_get(structured, "result"), "answer")), "yes") == 0);
    json_decref(response);
    return 0;
}

/* The host's `denied` refusal - sent for both a disallowed method and a
 * capability the client never declared, the same wire shape either way - must
 * surface as MAELYS_MCP_ERR_DENIED with the host's message intact. */
static int nested_denied(int fd, FILE *reader, unsigned long id,
    const char *method, const char *message) {
    ASSERT_TRUE(send_request(fd, id, "provider/call",
        json_pack("{s:s,s:{s:s}}", "name", "sdk.nested", "arguments", "method", method)));
    char nested_id[64];
    ASSERT_TRUE(read_nested_request(reader, method, nested_id, sizeof(nested_id)) == 0);

    json_t *reply = nested_reply_error(nested_id, "denied", message);
    ASSERT_TRUE(reply != NULL);
    ASSERT_TRUE(write_json_line(fd, reply));
    json_decref(reply);

    json_t *response = read_json_line(reader);
    ASSERT_TRUE(json_is_object(response));
    json_t *structured = json_object_get(json_object_get(response, "result"), "structuredContent");
    ASSERT_TRUE(json_integer_value(json_object_get(structured, "status")) ==
        (json_int_t)MAELYS_MCP_ERR_DENIED);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(structured, "error")), message) == 0);
    ASSERT_TRUE(json_is_null(json_object_get(structured, "result")));
    json_decref(response);
    return 0;
}

/* A reply that is valid JSON and a valid nestedReply envelope, but does not
 * carry the nestedId this call is waiting on - the shape the SDK must treat
 * as protocol corruption rather than accept or silently ignore. */
static int nested_malformed_reply(int fd, FILE *reader, unsigned long id) {
    ASSERT_TRUE(send_request(fd, id, "provider/call",
        json_pack("{s:s,s:{s:s}}", "name", "sdk.nested", "arguments", "method", "roots/list")));
    char nested_id[64];
    ASSERT_TRUE(read_nested_request(reader, "roots/list", nested_id, sizeof(nested_id)) == 0);

    json_t *reply = nested_reply_success("not-the-right-id", "nope");
    ASSERT_TRUE(reply != NULL);
    ASSERT_TRUE(write_json_line(fd, reply));
    json_decref(reply);

    json_t *response = read_json_line(reader);
    ASSERT_TRUE(json_is_object(response));
    json_t *structured = json_object_get(json_object_get(response, "result"), "structuredContent");
    ASSERT_TRUE(json_integer_value(json_object_get(structured, "status")) ==
        (json_int_t)MAELYS_MCP_ERR_PROTOCOL);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(structured, "error")),
        "the host sent a malformed nested reply") == 0);
    json_decref(response);
    return 0;
}

int main(void) {
    ASSERT_TRUE(test_options_default_stdout_isolation() == 0);
    ASSERT_TRUE(test_invalid_catalog_descriptors() == 0);
    int sockets[2] = {-1, -1};
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    sdk_fixture_t fixture = {
        .fd = sockets[1],
        .input_schema = json_pack("{s:s,s:{s:{s:s}},s:[s],s:b}",
            "type", "object", "properties", "message", "type", "string",
            "required", "message", "additionalProperties", 0),
        .output_schema = json_pack("{s:s,s:{s:{s:s}},s:[s],s:b}",
            "type", "object", "properties", "message", "type", "string",
            "required", "message", "additionalProperties", 0),
        .resource_schema = json_pack("{s:s}", "type", "object")
    };
    ASSERT_TRUE(fixture.input_schema && fixture.output_schema && fixture.resource_schema);
    ASSERT_TRUE(pthread_mutex_init(&fixture.producer_mutex, NULL) == 0);
    ASSERT_TRUE(pthread_cond_init(&fixture.producer_release, NULL) == 0);
    ASSERT_TRUE(pthread_create(&fixture.thread, NULL, serve_main, &fixture) == 0);
    FILE *reader = fdopen(sockets[0], "r+");
    ASSERT_TRUE(reader != NULL);

    ASSERT_TRUE(send_request(sockets[0], 1u, "provider/describe", NULL));
    json_t *response = read_json_line(reader);
    ASSERT_TRUE(json_is_object(response));
    /* Declared unconditionally, even before this provider has ever nested a
     * request: a /5 declaration that never nests must behave exactly as /4
     * did, so nothing here is gated on the provider having called
     * maelys_mcp_provider_sdk_request_client yet. */
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(response, "protocol")),
        MAELYS_MCP_PROVIDER_PROTOCOL) == 0);
    json_t *result = json_object_get(response, "result");
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(result, "name")), "sdk-test") == 0);
    ASSERT_TRUE(json_array_size(json_object_get(result, "tools")) == 5u);
    ASSERT_TRUE(json_array_size(json_object_get(result, "resources")) == 1u);
    json_decref(response);

    ASSERT_TRUE(send_request(sockets[0], 2u, "provider/activate", NULL));
    response = read_json_line(reader);
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);

    ASSERT_TRUE(send_request(sockets[0], 3u, "provider/call",
        json_pack("{s:s,s:{s:s}}", "name", "sdk.echo", "arguments", "message", "hello")));
    response = read_json_line(reader);
    result = json_object_get(response, "result");
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(result, "resultType")), "complete") == 0);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(
        json_object_get(result, "structuredContent"), "message")), "hello") == 0);
    json_decref(response);

    ASSERT_TRUE(send_request(sockets[0], 4u, "provider/readResource",
        json_pack("{s:s}", "uri", "sdk://about")));
    response = read_json_line(reader);
    result = json_object_get(response, "result");
    json_t *contents = json_object_get(result, "contents");
    ASSERT_TRUE(json_array_size(contents) == 1u);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(json_array_get(contents, 0), "text")),
        "SDK resource") == 0);
    json_decref(response);

    /* ---------------------------------------------------------- nested requests
     * The blocking helper, driven end to end over this same socketpair: the
     * SDK writes a real provider/nestedRequest and this thread plays the
     * host, answering (or refusing, or misbehaving) on the same connection.
     */
    ASSERT_TRUE(nested_round_trip(sockets[0], reader, 5u,
        "elicitation/create", "Proceed?") == 0);
    ASSERT_TRUE(nested_denied(sockets[0], reader, 6u,
        "tools/call", "method not allowed") == 0);
    ASSERT_TRUE(nested_denied(sockets[0], reader, 7u,
        "sampling/createMessage", "capability not declared") == 0);
    ASSERT_TRUE(nested_malformed_reply(sockets[0], reader, 8u) == 0);

    ASSERT_TRUE(send_request(sockets[0], 9u, "provider/call",
        json_pack("{s:s,s:{}}", "name", "sdk.events", "arguments")));
    json_t *event = read_json_line(reader);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(event, "method")),
        "provider/notifications/resources/updated") == 0);
    json_decref(event);
    event = read_json_line(reader);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(event, "method")),
        "provider/notifications/tools/list_changed") == 0);
    json_decref(event);
    response = read_json_line(reader);
    result = json_object_get(response, "result");
    ASSERT_TRUE(json_integer_value(json_object_get(
        json_object_get(result, "structuredContent"), "emitted")) == 2);
    json_decref(response);

    ASSERT_TRUE(send_request(sockets[0], 10u, "provider/call",
        json_pack("{s:s,s:{}}", "name", "sdk.async", "arguments")));
    json_t *first = read_json_line(reader);
    json_t *second = read_json_line(reader);
    ASSERT_TRUE(json_is_object(first) && json_is_object(second));
    json_t *async_response = json_is_integer(json_object_get(first, "id")) ? first : second;
    json_t *async_event = async_response == first ? second : first;
    ASSERT_TRUE(json_integer_value(json_object_get(async_response, "id")) == 10);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(async_event, "method")),
        "provider/notifications/resources/list_changed") == 0);
    json_decref(first);
    json_decref(second);

    ASSERT_TRUE(send_request(sockets[0], 11u, "provider/call",
        json_pack("{s:s,s:{}}", "name", "sdk.shutdown-race", "arguments")));
    response = read_json_line(reader);
    ASSERT_TRUE(json_integer_value(json_object_get(response, "id")) == 11);
    json_decref(response);

    ASSERT_TRUE(send_request(sockets[0], 12u, "provider/readResource",
        json_pack("{s:s}", "uri", "sdk://missing")));
    response = read_json_line(reader);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(
        json_object_get(response, "error"), "code")), "not_found") == 0);
    json_decref(response);

    ASSERT_TRUE(send_request(sockets[0], 13u, "provider/shutdown", NULL));
    response = read_json_line(reader);
    ASSERT_TRUE(json_is_object(json_object_get(response, "result")));
    json_decref(response);

    void *thread_result = NULL;
    ASSERT_TRUE(pthread_join(fixture.thread, &thread_result) == 0);
    ASSERT_TRUE((maelys_mcp_result_t)(intptr_t)thread_result == MAELYS_MCP_OK);
    ASSERT_TRUE(fixture.shutdown_calls == 1);
    ASSERT_TRUE(fixture.producer_started && fixture.producer_joined);
    ASSERT_TRUE(fixture.producer_status == MAELYS_MCP_OK);
    ASSERT_TRUE(fixture.late_producer_started && fixture.late_producer_joined);
    ASSERT_TRUE(fixture.late_producer_status == MAELYS_MCP_ERR_DENIED);
    pthread_cond_destroy(&fixture.producer_release);
    pthread_mutex_destroy(&fixture.producer_mutex);
    fclose(reader);
    close(sockets[1]);
    puts("test_provider_sdk: OK");
    return 0;
}
