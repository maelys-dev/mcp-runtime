#include "src/internal/internal.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * The runtime's half of the launch contract: the ownership rules, the
 * exactly-once teardown and the bounded ladder, implemented once for every
 * consumer. Deliberately free of process primitives - it only calls through
 * the vtable, which is what lets an OCI, a VM or an executord launcher fit
 * without a line changing here.
 */

static void set_launch_error(char **out_error, const char *message) {
    if (!out_error) return;
    free(*out_error);
    *out_error = maelys_mcp_strdup(message);
}

/*
 * Diagnostics about a launcher name it. The name is the launcher's own
 * (arbitrary) string, so the buffer is fixed and the format truncates rather
 * than sizing itself from untrusted input.
 */
static void set_launcher_error(
    char **out_error,
    const maelys_mcp_process_launcher_t *launcher,
    const char *message) {
    if (!out_error) return;
    char text[256];
    (void)snprintf(text, sizeof(text), "%s (launcher \"%s\")", message,
        launcher && launcher->name ? launcher->name : "unnamed");
    set_launch_error(out_error, text);
}

static int launcher_is_complete(const maelys_mcp_process_launcher_t *launcher) {
    return launcher && launcher->ops && launcher->ops->spawn &&
        launcher->ops->wait && launcher->ops->stop && launcher->ops->destroy;
}

/*
 * Cheap, once per provider, and it catches the realistic mistake - an
 * uninitialized or already-closed descriptor - without pretending to catch
 * every one.
 */
static int protocol_fd_is_usable(int fd) {
    return fd >= 0 && fcntl(fd, F_GETFD) >= 0;
}

/* stop -> wait, one rung. Reports whether the child was observed to exit. */
static int stop_and_wait(
    const maelys_mcp_process_launcher_t *launcher,
    void *handle,
    maelys_mcp_process_stop_t mode,
    unsigned int timeout_ms) {
    (void)launcher->ops->stop(launcher->context, handle, mode);
    int exited = 0;
    maelys_mcp_result_t status = launcher->ops->wait(launcher->context, handle,
        timeout_ms, &exited);
    return status == MAELYS_MCP_OK && exited;
}

/*
 * The one place destroy is called. instance_live is cleared BEFORE the call,
 * not after, so a destroy that itself re-enters teardown cannot reach a second
 * one - and so nullity of the handle, which is a legitimate value for a
 * stateless launcher, is never mistaken for "already destroyed".
 */
static void release_instance(
    const maelys_mcp_process_launcher_t *launcher,
    maelys_mcp_process_instance_t *instance) {
    void *handle = instance->handle;
    instance->instance_live = 0;
    instance->handle = NULL;
    launcher->ops->destroy(launcher->context, handle);
}

maelys_mcp_result_t maelys_mcp_process_launch(
    const maelys_mcp_process_launcher_t *launcher,
    const maelys_mcp_process_spec_t *spec,
    maelys_mcp_process_instance_t *out_instance,
    char **out_error) {
    if (!launcher_is_complete(launcher) || !spec || !out_instance) {
        set_launch_error(out_error, "process launcher is incomplete");
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    maelys_mcp_process_instance_t instance = {.protocol_fd = -1};
    maelys_mcp_result_t status = launcher->ops->spawn(launcher->context, spec,
        &instance, out_error);
    if (status != MAELYS_MCP_OK) return status;
    /*
     * From here the launch succeeded as far as the launcher is concerned, so
     * the child is the runtime's problem even when the runtime rejects what it
     * was handed. instance_live is set first: every path below tears down
     * through the ladder, and the ladder refuses to act on an instance whose
     * flag is clear.
     */
    instance.instance_live = 1;
    if (!protocol_fd_is_usable(instance.protocol_fd)) {
        /*
         * The one case where both sides could be right, resolved in the
         * runtime's favour because the runtime is the only side that can
         * detect it. Nothing valid to close, and the child is stopped rather
         * than leaked. Loud, because "the runtime survives a broken launcher"
         * is exactly the property a pluggable seam has to earn.
         */
        maelys_mcp_process_abandon(launcher, &instance, spec->force_timeout_ms);
        set_launcher_error(out_error, launcher,
            "process launcher returned an unusable protocol descriptor");
        return MAELYS_MCP_ERR_PROTOCOL;
    }
    *out_instance = instance;
    return MAELYS_MCP_OK;
}

maelys_mcp_result_t maelys_mcp_process_shutdown(
    const maelys_mcp_process_launcher_t *launcher,
    maelys_mcp_process_instance_t *instance,
    unsigned int grace_timeout_ms,
    unsigned int force_timeout_ms,
    char **out_error) {
    if (!launcher_is_complete(launcher) || !instance ||
        !instance->instance_live) {
        return MAELYS_MCP_OK;
    }
    void *handle = instance->handle;
    int exited = stop_and_wait(launcher, handle,
        MAELYS_MCP_PROCESS_STOP_GRACEFUL, grace_timeout_ms);
    if (!exited) {
        exited = stop_and_wait(launcher, handle,
            MAELYS_MCP_PROCESS_STOP_FORCED, force_timeout_ms);
    }
    release_instance(launcher, instance);
    if (exited) return MAELYS_MCP_OK;
    /*
     * The child outlived a forced stop. Something has to give, and the honest
     * options are a hang or a leak: this contract chooses the leak and
     * requires it to be reported. A leaked process is visible in ps,
     * survivable and attributable; a hung teardown is an unkillable host with
     * no message.
     */
    set_launcher_error(out_error, launcher,
        "child process survived a forced stop and was abandoned");
    return MAELYS_MCP_ERR_STATE;
}

void maelys_mcp_process_abandon(
    const maelys_mcp_process_launcher_t *launcher,
    maelys_mcp_process_instance_t *instance,
    unsigned int force_timeout_ms) {
    if (!launcher_is_complete(launcher) || !instance ||
        !instance->instance_live) {
        return;
    }
    (void)stop_and_wait(launcher, instance->handle,
        MAELYS_MCP_PROCESS_STOP_FORCED, force_timeout_ms);
    release_instance(launcher, instance);
}
