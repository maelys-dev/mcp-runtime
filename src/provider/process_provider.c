#include "src/internal/internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void replace_error(char **out_error, const char *message) {
    if (!out_error) return;
    free(*out_error);
    *out_error = maelys_mcp_strdup(message ? message : "provider error");
}

static maelys_mcp_result_t configure_timeout(int fd, unsigned int timeout_ms) {
    struct timeval timeout = {
        .tv_sec = (time_t)(timeout_ms / 1000u),
        .tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u)
    };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        return MAELYS_MCP_ERR_IO;
    }
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t process_exchange(
    maelys_mcp_process_context_t *process,
    const char *method,
    unsigned int timeout_ms,
    json_t *params,
    json_t **out_result,
    char **out_error) {
    if (!process || process->fd < 0 || !method || !out_result) return MAELYS_MCP_ERR_ARGUMENT;
    *out_result = NULL;
    if (configure_timeout(process->fd, timeout_ms) != MAELYS_MCP_OK) {
        replace_error(out_error, "cannot configure provider timeout");
        return MAELYS_MCP_ERR_IO;
    }
    unsigned long long id = ++process->next_id;
    json_t *request = json_object();
    if (!request) return MAELYS_MCP_ERR_MEMORY;
    if (json_object_set_new(request, "protocol", json_string(MAELYS_MCP_PROVIDER_PROTOCOL)) != 0 ||
        json_object_set_new(request, "id", json_integer((json_int_t)id)) != 0 ||
        json_object_set_new(request, "method", json_string(method)) != 0 ||
        json_object_set_new(request, "params", params ? json_incref(params) : json_object()) != 0) {
        json_decref(request);
        return MAELYS_MCP_ERR_MEMORY;
    }
    maelys_mcp_result_t status = maelys_mcp_write_json_line(process->fd, request);
    json_decref(request);
    if (status != MAELYS_MCP_OK) {
        replace_error(out_error, "cannot write to provider");
        return status;
    }

    json_t *response = NULL;
    status = maelys_mcp_line_reader_read(
        process->reader, process->fd, &response, out_error);
    if (status != MAELYS_MCP_OK) return status == MAELYS_MCP_ERR_NOT_FOUND ? MAELYS_MCP_ERR_PROVIDER : status;
    json_t *response_id = json_object_get(response, "id");
    json_t *protocol = json_object_get(response, "protocol");
    if (!json_is_integer(response_id) || (unsigned long long)json_integer_value(response_id) != id ||
        !maelys_mcp_json_string_equals(protocol, MAELYS_MCP_PROVIDER_PROTOCOL)) {
        json_decref(response);
        replace_error(out_error, "provider returned an invalid response envelope");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    json_t *error = json_object_get(response, "error");
    json_t *result = json_object_get(response, "result");
    if (!!error == !!result || (error && !json_is_object(error))) {
        json_decref(response);
        replace_error(out_error, "provider response must contain exactly one result or error");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    if (error) {
        json_t *message = json_object_get(error, "message");
        replace_error(out_error, json_is_string(message) &&
            !maelys_mcp_json_string_has_nul(message) ?
            json_string_value(message) : "provider call failed");
        json_decref(response);
        return MAELYS_MCP_ERR_PROVIDER;
    }
    *out_result = json_deep_copy(result);
    json_decref(response);
    return *out_result ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static maelys_mcp_result_t process_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    maelys_mcp_process_context_t *process = context;
    json_t *params = json_object();
    if (!params) return MAELYS_MCP_ERR_MEMORY;
    if (json_object_set_new(params, "name", json_string(request->tool_name)) != 0 ||
        json_object_set(params, "arguments", request->arguments) != 0 ||
        (request->input_responses &&
            json_object_set(params, "inputResponses", request->input_responses) != 0) ||
        (request->request_state &&
            json_object_set(params, "requestState", request->request_state) != 0) ||
        (request->client_capabilities &&
            json_object_set(params, "clientCapabilities", request->client_capabilities) != 0)) {
        json_decref(params);
        return MAELYS_MCP_ERR_MEMORY;
    }
    json_t *wire_result = NULL;
    maelys_mcp_result_t result = process_exchange(
        process, "provider/call", process->call_timeout_ms, params, &wire_result, out_error);
    json_decref(params);
    if (result != MAELYS_MCP_OK) return result;
    json_t *type = json_is_object(wire_result) ?
        json_object_get(wire_result, "resultType") : NULL;
    if (!json_is_string(type)) {
        json_decref(wire_result);
        replace_error(out_error, "provider resultType is missing");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    if (maelys_mcp_json_string_equals(type, "complete")) {
        out_result->type = MAELYS_MCP_PROVIDER_RESULT_COMPLETE;
    } else if (maelys_mcp_json_string_equals(type, "input_required")) {
        out_result->type = MAELYS_MCP_PROVIDER_RESULT_INPUT_REQUIRED;
    } else {
        json_decref(wire_result);
        replace_error(out_error, "provider resultType is unsupported");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    json_t *content = json_object_get(wire_result, "content");
    json_t *structured = json_object_get(wire_result, "structuredContent");
    json_t *input_requests = json_object_get(wire_result, "inputRequests");
    json_t *request_state = json_object_get(wire_result, "requestState");
    json_t *is_error = json_object_get(wire_result, "isError");
    if ((content && !json_is_array(content)) ||
        (input_requests && !json_is_object(input_requests)) ||
        (request_state && (!json_is_string(request_state) ||
            maelys_mcp_json_string_has_nul(request_state))) ||
        (is_error && !json_is_boolean(is_error))) {
        json_decref(wire_result);
        replace_error(out_error, "provider result fields have invalid types");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    out_result->content = content ? json_deep_copy(content) : NULL;
    out_result->structured_content = structured ? json_deep_copy(structured) : NULL;
    out_result->input_requests = input_requests ? json_deep_copy(input_requests) : NULL;
    out_result->request_state = request_state ? json_deep_copy(request_state) : NULL;
    out_result->is_error = json_is_true(is_error);
    if ((content && !out_result->content) || (structured && !out_result->structured_content) ||
        (input_requests && !out_result->input_requests) ||
        (request_state && !out_result->request_state)) {
        maelys_mcp_provider_result_clear(out_result);
        json_decref(wire_result);
        return MAELYS_MCP_ERR_MEMORY;
    }
    json_decref(wire_result);
    return result;
}

static long long monotonic_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (long long)now.tv_sec * 1000LL + (long long)now.tv_nsec / 1000000LL;
}

static int wait_for_child(pid_t pid, unsigned int timeout_ms) {
    long long start = monotonic_milliseconds();
    if (start < 0) return 0;
    for (;;) {
        int status = 0;
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid || (waited < 0 && errno == ECHILD)) return 1;
        if (waited < 0 && errno != EINTR) return 0;
        long long now = monotonic_milliseconds();
        if (now < 0 || now - start >= (long long)timeout_ms) return 0;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000L};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
}

static void process_destroy(void *context) {
    maelys_mcp_process_context_t *process = context;
    if (!process) return;
    if (process->fd >= 0) {
        json_t *ignored = NULL;
        char *error = NULL;
        (void)process_exchange(process, "provider/shutdown",
            process->shutdown_timeout_ms, NULL, &ignored, &error);
        if (ignored) json_decref(ignored);
        free(error);
        close(process->fd);
        process->fd = -1;
    }
    if (process->pid > 0 && !wait_for_child(process->pid, process->shutdown_timeout_ms)) {
        (void)kill(process->pid, SIGTERM);
        if (!wait_for_child(process->pid, process->shutdown_timeout_ms)) {
            (void)kill(process->pid, SIGKILL);
            while (waitpid(process->pid, NULL, 0) < 0 && errno == EINTR) {}
        }
    }
    if (process->reader) {
        maelys_mcp_line_reader_clear(process->reader);
        free(process->reader);
    }
    free(process);
}

static int set_close_on_exec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static maelys_mcp_result_t spawn_process(
    const maelys_mcp_provider_process_options_t *options,
    maelys_mcp_process_context_t **out_process,
    char **out_error) {
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        replace_error(out_error, "socketpair failed");
        return MAELYS_MCP_ERR_IO;
    }
    if (!set_close_on_exec(sockets[0]) || !set_close_on_exec(sockets[1])) {
        close(sockets[0]);
        close(sockets[1]);
        replace_error(out_error, "cannot protect provider descriptors");
        return MAELYS_MCP_ERR_IO;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(sockets[0]);
        close(sockets[1]);
        replace_error(out_error, "fork failed");
        return MAELYS_MCP_ERR_IO;
    }
    if (pid == 0) {
        close(sockets[0]);
        if (dup2(sockets[1], STDIN_FILENO) < 0 || dup2(sockets[1], STDOUT_FILENO) < 0) _exit(126);
        if (sockets[1] != STDIN_FILENO && sockets[1] != STDOUT_FILENO) close(sockets[1]);
        char *const argv[] = {(char *)options->executable_path, (char *)"--provider", NULL};
        char *const environment[] = {
#ifdef __APPLE__
            (char *)"PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin",
#else
            (char *)"PATH=/usr/local/bin:/usr/bin:/bin",
#endif
            (char *)"LANG=C",
            (char *)"LC_ALL=C",
            NULL
        };
        execve(options->executable_path, argv, environment);
        _exit(127);
    }
    close(sockets[1]);
#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    if (setsockopt(sockets[0], SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) != 0) {
        close(sockets[0]);
        (void)kill(pid, SIGTERM);
        (void)waitpid(pid, NULL, 0);
        replace_error(out_error, "cannot configure provider socket safety");
        return MAELYS_MCP_ERR_IO;
    }
#endif
    maelys_mcp_process_context_t *process = calloc(1u, sizeof(*process));
    if (!process) {
        close(sockets[0]);
        (void)kill(pid, SIGTERM);
        (void)waitpid(pid, NULL, 0);
        return MAELYS_MCP_ERR_MEMORY;
    }
    process->reader = calloc(1u, sizeof(*process->reader));
    if (!process->reader || maelys_mcp_line_reader_init(
        process->reader, options->max_message_bytes) != MAELYS_MCP_OK) {
        free(process->reader);
        free(process);
        close(sockets[0]);
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return MAELYS_MCP_ERR_MEMORY;
    }
    process->pid = pid;
    process->fd = sockets[0];
    process->max_message_bytes = options->max_message_bytes;
    process->describe_timeout_ms = options->describe_timeout_ms;
    process->call_timeout_ms = options->call_timeout_ms;
    process->shutdown_timeout_ms = options->shutdown_timeout_ms;
    *out_process = process;
    return MAELYS_MCP_OK;
}

maelys_mcp_result_t maelys_mcp_provider_spawn(
    const char *executable_path,
    size_t max_message_bytes,
    maelys_mcp_provider_t **out_provider,
    char **out_error) {
    maelys_mcp_provider_process_options_t options = {
        .executable_path = executable_path,
        .max_message_bytes = max_message_bytes,
        .describe_timeout_ms = MAELYS_MCP_DEFAULT_PROVIDER_DESCRIBE_TIMEOUT_MS,
        .call_timeout_ms = MAELYS_MCP_DEFAULT_PROVIDER_CALL_TIMEOUT_MS,
        .shutdown_timeout_ms = MAELYS_MCP_DEFAULT_PROVIDER_SHUTDOWN_TIMEOUT_MS
    };
    return maelys_mcp_provider_spawn_with_options(&options, out_provider, out_error);
}

maelys_mcp_result_t maelys_mcp_provider_spawn_with_options(
    const maelys_mcp_provider_process_options_t *options,
    maelys_mcp_provider_t **out_provider,
    char **out_error) {
    if (!options || !options->executable_path || options->executable_path[0] != '/' ||
        !out_provider) {
        replace_error(out_error, "provider executable must be an absolute path");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    maelys_mcp_provider_process_options_t normalized = *options;
    if (!normalized.max_message_bytes) {
        normalized.max_message_bytes = MAELYS_MCP_DEFAULT_MAX_MESSAGE_BYTES;
    }
    if (!normalized.describe_timeout_ms) {
        normalized.describe_timeout_ms = MAELYS_MCP_DEFAULT_PROVIDER_DESCRIBE_TIMEOUT_MS;
    }
    if (!normalized.call_timeout_ms) {
        normalized.call_timeout_ms = MAELYS_MCP_DEFAULT_PROVIDER_CALL_TIMEOUT_MS;
    }
    if (!normalized.shutdown_timeout_ms) {
        normalized.shutdown_timeout_ms = MAELYS_MCP_DEFAULT_PROVIDER_SHUTDOWN_TIMEOUT_MS;
    }
    *out_provider = NULL;
    maelys_mcp_process_context_t *process = NULL;
    maelys_mcp_result_t status = spawn_process(
        &normalized, &process, out_error);
    if (status != MAELYS_MCP_OK) return status;

    json_t *description = NULL;
    status = process_exchange(process, "provider/describe",
        process->describe_timeout_ms, NULL, &description, out_error);
    if (status != MAELYS_MCP_OK) {
        process_destroy(process);
        return status;
    }
    json_t *name = json_object_get(description, "name");
    json_t *version = json_object_get(description, "version");
    json_t *tools_json = json_object_get(description, "tools");
    if (!json_is_string(name) || maelys_mcp_json_string_has_nul(name) ||
        !json_is_string(version) || maelys_mcp_json_string_has_nul(version) ||
        !json_is_array(tools_json)) {
        json_decref(description);
        process_destroy(process);
        replace_error(out_error, "provider description is invalid");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    size_t count = json_array_size(tools_json);
    maelys_mcp_tool_t *tools = count ? calloc(count, sizeof(*tools)) : NULL;
    if (count && !tools) {
        json_decref(description);
        process_destroy(process);
        return MAELYS_MCP_ERR_MEMORY;
    }
    for (size_t index = 0; index < count; ++index) {
        json_t *tool = json_array_get(tools_json, index);
        json_t *tool_name = json_object_get(tool, "name");
        json_t *title = json_object_get(tool, "title");
        json_t *tool_description = json_object_get(tool, "description");
        json_t *input_schema = json_object_get(tool, "inputSchema");
        json_t *output_schema = json_object_get(tool, "outputSchema");
        json_t *effect = json_object_get(tool, "effect");
        maelys_mcp_tool_effect_t parsed_effect;
        if (!json_is_string(tool_name) || maelys_mcp_json_string_has_nul(tool_name) ||
            !json_is_string(tool_description) || maelys_mcp_json_string_has_nul(tool_description) ||
            (title && (!json_is_string(title) || maelys_mcp_json_string_has_nul(title))) ||
            !json_is_object(input_schema) || (output_schema && !json_is_object(output_schema)) ||
            !json_is_string(effect) || maelys_mcp_json_string_has_nul(effect) ||
            maelys_mcp_tool_effect_parse(
                json_string_value(effect), &parsed_effect) != MAELYS_MCP_OK) {
            free(tools);
            json_decref(description);
            process_destroy(process);
            replace_error(out_error, "provider tool description is invalid");
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        char *schema_error = NULL;
        if (maelys_mcp_validate_schema_definition(input_schema, 1, &schema_error) != MAELYS_MCP_OK ||
            (output_schema &&
             maelys_mcp_validate_schema_definition(output_schema, 0, &schema_error) != MAELYS_MCP_OK)) {
            free(tools);
            json_decref(description);
            process_destroy(process);
            replace_error(out_error, schema_error ? schema_error : "provider schema is unsupported");
            free(schema_error);
            return MAELYS_MCP_ERR_PROTOCOL;
        }
        free(schema_error);
        tools[index] = (maelys_mcp_tool_t){
            .name = json_string_value(tool_name),
            .title = json_is_string(title) ? json_string_value(title) : json_string_value(tool_name),
            .description = json_string_value(tool_description),
            .input_schema = input_schema,
            .output_schema = output_schema,
            .effect = parsed_effect
        };
    }
    maelys_mcp_provider_config_t config = {
        .name = json_string_value(name),
        .version = json_string_value(version),
        .tools = tools,
        .tool_count = count,
        .call = process_call,
        .destroy = process_destroy,
        .context = process
    };
    status = maelys_mcp_provider_create(&config, out_provider);
    free(tools);
    json_decref(description);
    if (status != MAELYS_MCP_OK) process_destroy(process);
    return status;
}
