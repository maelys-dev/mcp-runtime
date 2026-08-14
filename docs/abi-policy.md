# C API and ABI policy

The public C API uses opaque handles for every stateful object:
`maelys_mcp_runtime_t`, `maelys_mcp_provider_t`, `maelys_mcp_outbox_t` and
`maelys_uri_t`. Their definitions are private and will remain private.

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
- the private `maelys-provider/3` process protocol is versioned separately and does not
  share the native C ABI.

The build exports a static library today. This policy also applies if a shared-library
artifact is introduced later.
