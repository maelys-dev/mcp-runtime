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

ABI 3 was introduced by version 0.14.0. It removes `authorize`, `audit` and
`policy_context` from `maelys_mcp_runtime_config_t`: the middleware chain
(`maelys/mcp/middleware.h`) replaces those callbacks, and
`maelys_mcp_runtime_add_compat_policy` wraps a legacy callback pair as a
built-in middleware for one-call migration. Later additions within ABI 3
(nested-request entry points, the transformation-hook descriptor fields)
were additive: new structs and new entry points rather than changes to
released layouts, which is this project's preferred idiom for evolving the
API without an ABI break.
