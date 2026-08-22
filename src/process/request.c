#include "src/internal/internal.h"

/*
 * The opaque launch request's getters: the whole of what a launcher is allowed
 * to know about a launch.
 *
 * There is nothing here but reads, deliberately. The request is compiled by
 * maelys_mcp_process_launch (src/process/launcher.c) and lives on that
 * function's stack, so these run only while a spawn call is on the stack above
 * them, and every pointer they hand back dies with that frame - which is the
 * lifetime rule the public header states once and does not repeat at each
 * getter.
 *
 * None of them checks `request` for NULL. That is not an oversight: the header
 * states that the runtime never hands a launcher a NULL request and that these
 * getters have no error channel with which to say it did, so a guard here
 * could only invent a return value for a case the contract forbids. The
 * request reaches a launcher one way - as the argument of the spawn call the
 * runtime just made - and the runtime is the only thing that builds one.
 */

const char *maelys_mcp_process_request_executable(
    const maelys_mcp_process_request_t *request) {
    return request->executable;
}

size_t maelys_mcp_process_request_arg_count(
    const maelys_mcp_process_request_t *request) {
    return request->arg_count;
}

const char *maelys_mcp_process_request_arg_at(
    const maelys_mcp_process_request_t *request, size_t index) {
    if (index >= request->arg_count) return NULL;
    return request->argv[index];
}

const char *maelys_mcp_process_request_execution_profile(
    const maelys_mcp_process_request_t *request) {
    return request->execution_profile;
}

maelys_mcp_process_fd_layout_t maelys_mcp_process_request_fd_layout(
    const maelys_mcp_process_request_t *request) {
    return request->fd_layout;
}

unsigned int maelys_mcp_process_request_spawn_timeout_ms(
    const maelys_mcp_process_request_t *request) {
    return request->spawn_timeout_ms;
}

unsigned int maelys_mcp_process_request_grace_timeout_ms(
    const maelys_mcp_process_request_t *request) {
    return request->grace_timeout_ms;
}

unsigned int maelys_mcp_process_request_force_timeout_ms(
    const maelys_mcp_process_request_t *request) {
    return request->force_timeout_ms;
}

size_t maelys_mcp_process_request_environment_count(
    const maelys_mcp_process_request_t *request) {
    return request->environment_count;
}

int maelys_mcp_process_request_environment_at(
    const maelys_mcp_process_request_t *request,
    size_t index,
    const char **out_name,
    const char **out_value) {
    if (!out_name || !out_value) return 0;
    /* Neither out parameter is written for an index past the end, which is
     * what lets a launcher walk the list with a bare loop and no bounds
     * arithmetic of its own. */
    if (index >= request->environment_count) return 0;
    *out_name = request->environment_names[index];
    *out_value = request->environment_values[index];
    return 1;
}

const char *maelys_mcp_process_request_environment_platform(
    const maelys_mcp_process_request_t *request) {
    return request->environment_platform;
}
