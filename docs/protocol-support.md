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

The current milestone exposes modular Tools and Resources facades, Multi Round-Trip
Requests and modern Subscriptions. Resources supports static lists, URI templates and reads. Complete tool
results support text, image, audio, resource links, embedded resources and structured
content. MRTR supports `elicitation/create`,
`sampling/createMessage`, and `roots/list` input requests with client-capability
enforcement and provider-owned opaque state.

With the Subscriptions module enabled, `subscriptions/listen` negotiates the supported
subset of `toolsListChanged`, `resourcesListChanged` and `resourceSubscriptions`.
The acknowledgement is always first. Accepted events carry
`_meta["io.modelcontextprotocol/subscriptionId"]`; `notifications/cancelled` removes
the listen request, while transport shutdown emits a final `resultType: "complete"`
response. URI filters are canonicalized and match the exact resource or a descendant.
Prompt list-change filters are syntactically checked but not accepted because Prompts
is not implemented.

Prompts, Tasks, progress, provider-originated asynchronous events, Streamable HTTP,
HTTP header routing, and authorization transports are not implemented. Resource *content blocks* returned
by a tool remain distinct from the independently enabled MCP Resources capability.

The upstream conformance runner currently requires an HTTP server and its complete
2026-07-28 requirement set covers capabilities beyond this milestone. Its current
HTTP request/response adapter cannot model a long-lived subscription stream. The repository
therefore runs a documented, pinned subset through a test-only HTTP-to-stdio adapter;
see [Official MCP conformance](official-conformance.md). This validates the supported
wire surface without claiming complete protocol conformance.
