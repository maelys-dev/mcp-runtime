# C API and ABI policy

The public C API uses opaque handles for every stateful object:
`maelys_mcp_runtime_t`, `maelys_mcp_channel_t`, `maelys_mcp_provider_t`,
`maelys_mcp_outbox_t` and `maelys_uri_t`. Their definitions are private and will
remain private.

Descriptors, callback requests, callback results, configuration records and statistics
are deliberately public plain-data structures. They make provider integration simple,
but their layout is part of the binary interface once released.

`MAELYS_MCP_ABI_VERSION` identifies that binary interface independently from the
package version. `maelys_mcp_abi_version()` lets an embedding application verify the
loaded library, while `maelys_mcp_version_string()` reports the package version.

The policy is:

- patch releases preserve both source and binary compatibility;
- before 1.0, a minor release may make an incompatible API or ABI change, but it must
  increment `MAELYS_MCP_ABI_VERSION` and document the migration in `CHANGELOG.md`;
- from 1.0, releases with the same major package version preserve the public ABI on a
  given supported platform and architecture;
- adding fields directly to an existing public structure is an ABI break; a compatible
  extension uses a new structure or a new versioned constructor;
- public stateful objects never expose their storage layout;
- the private `maelys-provider` process protocol is versioned separately, is
  negotiated per provider rather than fixed, and does not share the native C ABI.

The build exports a static library today. This policy also applies if a shared-library
artifact is introduced later.

ABI 2 was introduced by version 0.10.0. It intentionally removes the ABI 1
runtime-wide dispatch/outbox attachment surface. ABI 2 embedders create one or more
channels, enqueue requests through `maelys_mcp_channel_handle`, pump ordered output
with `maelys_mcp_channel_next`, and destroy every channel before destroying its
runtime. No ABI 1 compatibility wrappers are exported.

The process launch seam (`maelys/mcp/process_launcher.h`,
`maelys_mcp_provider_spawn_with_launcher` and its proxy twin) is the most recent
addition made that way: new structures reached by new entry points, with every
existing entry point preserved as a wrapper that binds the POSIX launcher, so no
released layout changed and `MAELYS_MCP_ABI_VERSION` did not move. None of its
structures carries a `struct_size` field, and none will: this policy's answer to
extension is a new structure or a new versioned constructor, and a second mechanism
would buy silent tolerance of mismatched builds in place of the link error this
policy prefers.

ABI 4 replaces the 0.17.0 protocol-era setter with two additions to
`maelys_mcp_channel_config_t`, and the migration is a recompile for anyone who
never restricted eras.

- **`protocol_eras`, and the removal of `maelys_mcp_channel_set_protocol_eras`.**
  Set `.protocol_eras` in the config already passed to
  `maelys_mcp_channel_create` instead of calling the setter after it. Zero means
  `MAELYS_MCP_ERA_ALL`, so a zero-initialized config behaves exactly as it did
  before; a non-zero mask carrying a bit outside `MAELYS_MCP_ERA_ALL` is
  `MAELYS_MCP_ERR_ARGUMENT` from `maelys_mcp_channel_create` and no channel is
  created. The setter's state rules disappear with it: there is no longer a
  window in which a channel's era set can be narrowed after traffic has
  started, and therefore no "cannot withdraw `MAELYS_MCP_ERA_LEGACY` after
  `initialize`" case and no closing-channel refusal to handle.
- **`context_release` and `release_context`: the channel context can own
  something.** When `context_release` is set it is called exactly once, with
  `release_context` and `context`, at the moment the context stops being
  reachable - on the synchronous destroy path and on
  `maelys_mcp_channel_destroy_detached`'s deferred one alike, on whichever
  thread performs the real free. NULL preserves the previous behaviour, in
  which the runtime owns nothing. This closes the gap 0.17.0's detached
  destruction left open, where an embedder had to wait for
  `maelys_mcp_runtime_destroy` to reclaim a detached channel's context.

The break is taken now for one stated reason - there are no external users, so
the migration costs a recompile - and its scope is deliberately these fields
and this removal. The structural changes an ABI bump would otherwise be saved
for (typed identity, cancellation policy, transport options) are not in it and
remain on the 1.0 runway. See `docs/authenticated-principal-design.md` for the
ownership argument behind `context_release`.

ABI 5 revises the process launch seam, and is the first change to it that could
not use the new-structure-and-new-entry-point idiom: `maelys_mcp_process_spec_t`
and `maelys_mcp_process_instance_t` are removed rather than extended, and
`maelys_mcp_process_ops_t` changes shape, so the constant moves. The spec
becomes the opaque `maelys_mcp_process_request_t` read through getters — which
restores the idiom for everything after this, since a getter is a new entry
point — the instance struct is replaced by `spawn` out-parameters, `wait`
reports a full `maelys_mcp_process_exit_status_t`, the launcher becomes an
opaque refcounted handle created by `maelys_mcp_process_launcher_create`, and
the child environment travels through the seam under an explicit
platform rule. `maelys_mcp_process_ops_t` leads with an `abi_version` field
that `maelys_mcp_process_launcher_create` checks for exact equality against
`MAELYS_MCP_ABI_VERSION`, refusing any mismatch: an ops table is populated at
run time by a separately compiled launcher, so it links cleanly and then calls
through a function pointer at the wrong offset, and the check is what restores
the link error this policy prefers at the one place a link error cannot occur.
Still no `struct_size`, for the reason stated above. `docs/launch-contract-design.md`,
"ABI 5 — the launcher contract", carries the full contract and the argument for
each part of it.

ABI 3 was introduced by version 0.14.0. It removes `authorize`, `audit` and
`policy_context` from `maelys_mcp_runtime_config_t`: the middleware chain
(`maelys/mcp/middleware.h`) replaces those callbacks, and
`maelys_mcp_runtime_add_compat_policy` wraps a legacy callback pair as a
built-in middleware for one-call migration. Later additions within ABI 3
(nested-request entry points, the transformation-hook descriptor fields)
were additive: new structs and new entry points rather than changes to
released layouts, which is this project's preferred idiom for evolving the
API without an ABI break.
