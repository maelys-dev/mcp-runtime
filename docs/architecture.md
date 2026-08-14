# Architecture

## Purpose

The runtime is a protocol and policy boundary shared by Maelys, projectctl, Hermes,
and future developer tools. Each tool keeps its business logic and native language.

```text
MCP client
    |
    v
maelys-mcp host
    |- JSON-RPC and MCP validation
    |- policy and audit hooks
    |- tool catalog and routing
    |
    +--> in-process C provider
    +--> persistent Python provider
    +--> persistent TypeScript provider
```

## Dependency rule

The runtime must not include headers or source files from codexmanager, projectctl,
Hermes, or any provider. Dependencies point toward the public provider contract, never
toward application implementations.

The TypeScript and Python SDK packages implement that public contract but contain no
application tools. Providers depend on an SDK; the native runtime never depends on an
SDK or application package.

Codex and Claude are MCP clients, not provider implementations. Their integration is a
small launch configuration that starts the same `maelys-mcp` stdio host; no compiled
client-specific adapter belongs in this library.

## Ownership

- `maelys_mcp_runtime_add_provider` transfers provider ownership on success.
- The caller retains ownership when registration fails.
- Provider descriptors are deep-copied by `maelys_mcp_provider_create`.
- Provider callbacks return a newly owned `json_t` result and an optional allocated
  error string. The runtime releases both.
- `maelys_mcp_runtime_handle` borrows the request and returns a newly owned response.

## Protocol eras

The same dispatcher supports:

- modern MCP `2026-07-28`, with per-request metadata and no session;
- legacy MCP `2025-11-25`, with `initialize` for compatible clients.

Modern behavior is selected by
`params._meta["io.modelcontextprotocol/protocolVersion"]`.
Modern final results carry `resultType: "complete"`, server identity metadata, and
conservative cache hints on cacheable list/discovery operations. Multi-round-trip
`input_required` results are intentionally deferred to a later milestone.

## Current JSON Schema subset

The runtime validates `type`, `properties`, `required`, `additionalProperties`,
`items`, `enum`, `minLength`, `maxLength`, `minimum`, and `maximum`. Providers remain
responsible for their domain invariants. `$schema`, `title`, and `description` are
accepted as annotations. Every schema node must declare one supported `type`; tool
input schemas must have an object root. Unsupported validation keywords and malformed
definitions are rejected before a provider is registered, so the schema exposed by
`tools/list` cannot promise a constraint that runtime validation silently ignores.
Full JSON Schema 2020-12 is a later milestone.
