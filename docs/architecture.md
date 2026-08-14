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
    |- module registry
    |   |- Tools
    |   `- MRTR
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
- Provider callbacks fill one initialized `maelys_mcp_provider_result_t`. Every JSON
  field placed in it is a newly owned Jansson reference. The runtime releases all
  fields with `maelys_mcp_provider_result_clear`.
- `maelys_mcp_runtime_handle` borrows the request and returns a newly owned response.

## Protocol eras

The same dispatcher supports:

- modern MCP `2026-07-28`, with per-request metadata and no session;
- legacy MCP `2025-11-25`, with `initialize` for compatible clients.

Modern behavior is selected by
`params._meta["io.modelcontextprotocol/protocolVersion"]`.
Modern final results carry `resultType: "complete"`, server identity metadata, and
conservative cache hints on cacheable list/discovery operations. When the MRTR module
is enabled, a provider may instead return `input_required`; the retry's
`inputResponses`, opaque `requestState`, and client capabilities are passed back to
the same provider.

## Module boundary

The core owns lifecycle, version negotiation, discovery and generic JSON-RPC routing.
It does not contain method-name branches for `tools/list` or `tools/call`. Active
modules publish their capabilities and claim their methods through the internal
registry. A newly created runtime has no application capability; the host explicitly
enables Tools and MRTR. Adding a provider before enabling Tools fails closed.

MRTR deliberately remains separate from Tools even though the first supported use is
`tools/call`: Tools owns catalog and execution semantics; MRTR owns permission for
multi-round results and retry fields. Future Prompts or Resources modules can reuse
MRTR without copying it.

## Current JSON Schema subset

The runtime validates `type`, `properties`, `required`, `additionalProperties`,
`items`, `enum`, `minLength`, `maxLength`, `minimum`, and `maximum`. Providers remain
responsible for their domain invariants. `$schema`, `title`, and `description` are
accepted as annotations. Every schema node must declare one supported `type`; tool
input schemas must have an object root. Unsupported validation keywords and malformed
definitions are rejected before a provider is registered, so the schema exposed by
`tools/list` cannot promise a constraint that runtime validation silently ignores.
Full JSON Schema 2020-12 is a later milestone.
