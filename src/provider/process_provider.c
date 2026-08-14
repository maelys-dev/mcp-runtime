#include "src/internal/internal.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static void replace_error(char **out_error, const char *message) {
    if (!out_error) return;
    free(*out_error);
    *out_error = maelys_mcp_strdup(message ? message : "provider error");
}

static maelys_mcp_result_t process_exchange(
    maelys_mcp_process_context_t *process,
    const char *method,
    json_t *params,
    json_t **out_result,
    char **out_error) {
    if (!process || process->fd < 0 || !method || !out_result) return MAELYS_MCP_ERR_ARGUMENT;
    *out_result = NULL;
    unsigned long long id = ++process->next_id;
    json_t *request = json_object();
    if (!request) return MAELYS_MCP_ERR_MEMORY;
    if (json_object_set_new(request, "protocol", json_string("maelys-provider/1")) != 0 ||
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
    status = maelys_mcp_read_json_line(process->fd, process->max_message_bytes, &response, out_error);
    if (status != MAELYS_MCP_OK) return status == MAELYS_MCP_ERR_NOT_FOUND ? MAELYS_MCP_ERR_PROVIDER : status;
    json_t *response_id = json_object_get(response, "id");
    json_t *protocol = json_object_get(response, "protocol");
    if (!json_is_integer(response_id) || (unsigned long long)json_integer_value(response_id) != id ||
        !json_is_string(protocol) || strcmp(json_string_value(protocol), "maelys-provider/1") != 0) {
        json_decref(response);
        replace_error(out_error, "provider returned an invalid response envelope");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    json_t *error = json_object_get(response, "error");
    if (json_is_object(error)) {
        json_t *message = json_object_get(error, "message");
        replace_error(out_error, json_is_string(message) ? json_string_value(message) : "provider call failed");
        json_decref(response);
        return MAELYS_MCP_ERR_PROVIDER;
    }
    json_t *result = json_object_get(response, "result");
    if (!result) {
        json_decref(response);
        replace_error(out_error, "provider response has no result");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    *out_result = json_deep_copy(result);
    json_decref(response);
    return *out_result ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static maelys_mcp_result_t process_call(
    void *context,
    const char *tool_name,
    json_t *arguments,
    json_t **out_result,
    char **out_error) {
    maelys_mcp_process_context_t *process = context;
    json_t *params = json_object();
    if (!params) return MAELYS_MCP_ERR_MEMORY;
    if (json_object_set_new(params, "name", json_string(tool_name)) != 0 ||
        json_object_set(params, "arguments", arguments) != 0) {
        json_decref(params);
        return MAELYS_MCP_ERR_MEMORY;
    }
    maelys_mcp_result_t result = process_exchange(
        process, "provider/call", params, out_result, out_error);
    json_decref(params);
    return result;
}

static void process_destroy(void *context) {
    maelys_mcp_process_context_t *process = context;
    if (!process) return;
    if (process->fd >= 0) {
        json_t *ignored = NULL;
        char *error = NULL;
        (void)process_exchange(process, "provider/shutdown", NULL, &ignored, &error);
        if (ignored) json_decref(ignored);
        free(error);
        close(process->fd);
        process->fd = -1;
    }
    if (process->pid > 0) {
        int status = 0;
        pid_t waited;
        do { waited = waitpid(process->pid, &status, WNOHANG); } while (waited < 0 && errno == EINTR);
        if (waited == 0) {
            (void)kill(process->pid, SIGTERM);
            do { waited = waitpid(process->pid, &status, 0); } while (waited < 0 && errno == EINTR);
        }
    }
    free(process);
}

static maelys_mcp_result_t spawn_process(
    const char *path,
    size_t max_message_bytes,
    maelys_mcp_process_context_t **out_process,
    char **out_error) {
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        replace_error(out_error, "socketpair failed");
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
        char *const argv[] = {(char *)path, (char *)"--provider", NULL};
        char *const environment[] = {
            (char *)"PATH=/usr/bin:/bin",
            (char *)"LANG=C",
            (char *)"LC_ALL=C",
            NULL
        };
        execve(path, argv, environment);
        _exit(127);
    }
    close(sockets[1]);
    struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
    if (setsockopt(sockets[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(sockets[0], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        close(sockets[0]);
        (void)kill(pid, SIGTERM);
        (void)waitpid(pid, NULL, 0);
        replace_error(out_error, "cannot configure provider timeout");
        return MAELYS_MCP_ERR_IO;
    }
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
    process->pid = pid;
    process->fd = sockets[0];
    process->max_message_bytes = max_message_bytes;
    *out_process = process;
    return MAELYS_MCP_OK;
}

maelys_mcp_result_t maelys_mcp_provider_spawn(
    const char *executable_path,
    size_t max_message_bytes,
    maelys_mcp_provider_t **out_provider,
    char **out_error) {
    if (!executable_path || executable_path[0] != '/' || !out_provider) {
        replace_error(out_error, "provider executable must be an absolute path");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    if (max_message_bytes == 0) max_message_bytes = MAELYS_MCP_DEFAULT_MAX_MESSAGE_BYTES;
    *out_provider = NULL;
    maelys_mcp_process_context_t *process = NULL;
    maelys_mcp_result_t status = spawn_process(
        executable_path, max_message_bytes, &process, out_error);
    if (status != MAELYS_MCP_OK) return status;

    json_t *description = NULL;
    status = process_exchange(process, "provider/describe", NULL, &description, out_error);
    if (status != MAELYS_MCP_OK) {
        process_destroy(process);
        return status;
    }
    json_t *name = json_object_get(description, "name");
    json_t *version = json_object_get(description, "version");
    json_t *tools_json = json_object_get(description, "tools");
    if (!json_is_string(name) || !json_is_string(version) || !json_is_array(tools_json)) {
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
        if (!json_is_string(tool_name) || !json_is_string(tool_description) ||
            !json_is_object(input_schema) || (output_schema && !json_is_object(output_schema)) ||
            !json_is_string(effect) || maelys_mcp_tool_effect_parse(
                json_string_value(effect), &parsed_effect) != MAELYS_MCP_OK) {
            free(tools);
            json_decref(description);
            process_destroy(process);
            replace_error(out_error, "provider tool description is invalid");
            return MAELYS_MCP_ERR_PROTOCOL;
        }
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
