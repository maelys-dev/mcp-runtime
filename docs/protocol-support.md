# MCP protocol support

The runtime implements two protocol eras:

- `2026-07-28`: stateless per-request metadata, `server/discover`, server identity in
  result metadata, mandatory final `resultType`, and conservative cache hints;
- `2025-11-25`: the `initialize` / `notifications/initialized` lifecycle for existing
  stdio clients.

The modern implementation follows the final specification rather than its earlier
release-candidate wire shape. Primary references:

- <https://blog.modelcontextprotocol.io/posts/2026-07-28/>
- <https://ts.sdk.modelcontextprotocol.io/v2/migration/support-2026-07-28>

The current milestone exposes tools only. Prompts, resources, subscriptions,
Multi Round-Trip Requests, Tasks, HTTP header routing, and authorization transports
are not yet implemented.

The upstream conformance runner currently requires an HTTP server and its complete
2026-07-28 requirement set covers capabilities beyond this milestone. The repository
therefore runs a documented, pinned subset through a test-only HTTP-to-stdio adapter;
see [Official MCP conformance](official-conformance.md). This validates the supported
wire surface without claiming complete protocol conformance.
