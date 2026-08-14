# Provenance

The JSON-RPC framing core originates from the user-owned `codexmanager` repository:

```text
codexmanager/transports/jsonrpc_transport_core.c
codexmanager/transports/jsonrpc_transport_core.h
codexmanager/tests/test_jsonrpc_transport.c
```

The extraction is based on source commit
`a4f7b2c5f22f2cef22666c8ac58719a2109a02f0` (`feat: extract jsonrpc transport core`).
This pins the lineage independently of later uncommitted work in the source repository.

It was extracted into `src/jsonrpc/core.c`, renamed to the standalone public namespace,
and adapted from `maelys_result_t` and the codexmanager transport configuration to
`maelys_mcp_result_t` and a focused framing configuration.

The JSON-RPC success/error envelope in `src/core/common.c` is also adapted from
`protocols/mcp/mcp_error.c`; the standalone version adds structured error data. The
generic ownership, lifecycle, framing, error, stdio, CLI, and notification cases from
the JSON-RPC and MCP test families were retained as runtime regressions, while the
application-specific binding itself was replaced by the provider-neutral dispatcher.
The exact inclusion/exclusion matrix is documented in `docs/test-parity.md`.

The following codexmanager code was intentionally not copied because it crosses an
application boundary:

```text
protocols/mcp/mcp_registry.*
protocols/mcp/mcp_schema.*
protocols/mcp/mcp_server.*
protocols/mcp/mcp_endpoint_runtime_server.*
protocols/mcp/mcp_stdio_server.*
```

Their Maelys policies, L0/L1 operations, agent endpoints and workflow handlers belong
in the future codexmanager provider adapter.
