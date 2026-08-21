#pragma once

#include "maelys/mcp/process_launcher.h"

/*
 * The runtime's half of the launch contract. The seam's types and the POSIX
 * launcher are public (maelys/mcp/process_launcher.h); the three functions
 * below are not, because they are how *this* runtime holds up its end -
 * ownership, exactly-once teardown, and the bounded ladder - rather than
 * anything an embedder implements or calls.
 *
 * They contain no process primitives themselves, only calls through the
 * vtable, which is what lets a container or an executord launcher fit without
 * a line changing in them.
 */

/*
 * spawn, plus the obligations the runtime owes itself: instance_live is set
 * here and nowhere else, and protocol_fd is checked for usability before any
 * caller can touch it.
 *
 * A launcher that returns MAELYS_MCP_OK with an unusable protocol_fd has
 * violated the contract in the one way only the runtime can detect. That is
 * resolved in the runtime's favour: the child is stopped and released, and the
 * launch fails with MAELYS_MCP_ERR_PROTOCOL naming the launcher.
 */
maelys_mcp_result_t maelys_mcp_process_launch(
    const maelys_mcp_process_launcher_t *launcher,
    const maelys_mcp_process_spec_t *spec,
    maelys_mcp_process_instance_t *out_instance,
    char **out_error);

/*
 * The bounded shutdown ladder, every rung budgeted:
 *
 *     stop(GRACEFUL) -> wait(grace_timeout_ms) -> stop(FORCED)
 *                    -> wait(force_timeout_ms) -> destroy
 *
 * The kind-specific goodbye - a provider/shutdown exchange for a native
 * provider, a half-close for an MCP upstream - happens ABOVE this call and
 * stays unmerged: those are protocol facts, not launch facts.
 *
 * Returns MAELYS_MCP_OK when the child was observed to exit, and
 * MAELYS_MCP_ERR_STATE with a diagnostic in *out_error when it outlived a
 * forced stop. That second case is a containment failure and a genuinely bad
 * outcome; the contract chooses the leak over the hang and requires it to be
 * reported. destroy is called either way, exactly once, and this function
 * always returns.
 *
 * A no-op returning MAELYS_MCP_OK when instance_live is already clear.
 */
maelys_mcp_result_t maelys_mcp_process_shutdown(
    const maelys_mcp_process_launcher_t *launcher,
    maelys_mcp_process_instance_t *instance,
    unsigned int grace_timeout_ms,
    unsigned int force_timeout_ms,
    char **out_error);

/*
 * The failure-path form of the ladder: stop(FORCED) -> wait -> destroy, with
 * no graceful rung. Used where a launch has already failed for a reason of the
 * runtime's own (an allocation, a thread that would not start) and the child
 * is seconds old, has produced nothing to flush, and must not be given the
 * chance to ignore a polite request - the rule PR #53 established for exactly
 * these paths. Void, because these callers already have the error they are
 * returning.
 */
void maelys_mcp_process_abandon(
    const maelys_mcp_process_launcher_t *launcher,
    maelys_mcp_process_instance_t *instance,
    unsigned int force_timeout_ms);
