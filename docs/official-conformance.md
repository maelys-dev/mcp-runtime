# Official MCP conformance

The upstream `@modelcontextprotocol/conformance` server runner accepts only an HTTP
URL. Maelys MCP Runtime 0.2.x intentionally exposes a local stdio transport, so the
repository includes an HTTP-to-stdio adapter used only by tests. It is not installed
and is not a supported runtime transport.

Run the official scenarios that match the current tools-only contract:

```sh
make test-mcp-conformance-official
```

The package version is pinned through `MCP_CONFORMANCE_PACKAGE`. The selected scenarios
exercise discovery through the official client, validate wire messages against the
2026-07-28 schema, list tools, call a successful text tool, and check MCP tool errors.

This is deliberately a partial conformance result. The frozen 2026-07-28 requirements
also mandate resources, prompts, request-scoped SSE, subscriptions and Multi Round-Trip
Requests. The runtime does not implement or advertise those capabilities, and therefore
does not claim complete MCP 2026-07-28 conformance.

The private `maelys-provider/1` protocol has a separate black-box runner in
`conformance/provider_conformance.py`. The TypeScript and Python packages under `sdk/`
are provider SDKs only; they are not general MCP client or server SDKs.

```sh
make test-provider-conformance
```
