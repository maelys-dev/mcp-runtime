# MCP protocol support

The runtime implements two protocol eras:

- `2026-07-28`: stateless per-request metadata, `server/discover`, server identity in
  result metadata, mandatory final `resultType`, and conservative cache hints;
- the legacy `initialize` / `notifications/initialized` lifecycle for existing stdio
  clients. Every dated revision up to `2025-11-25` shares that handshake, so the
  runtime accepts `2024-11-05`, `2025-03-26`, `2025-06-18` and `2025-11-25`, and — per
  MCP version negotiation — echoes the client's requested version back in the
  `initialize` result. `2026-07-28` removed `initialize`; a client that sends it is by
  definition using the legacy era.

A channel serves both eras unless the embedder narrows it with
`maelys_mcp_channel_set_protocol_eras`, which is a per-channel decision rather than a
per-runtime or per-transport one — one runtime can serve a dual-era stdio channel and
a modern-only channel simultaneously. A narrowed channel announces only the eras it
serves in `server/discover`, refuses `initialize` with `-32600` when the legacy era is
cleared, and refuses modern `_meta` negotiation with `-32022` when the modern era is
cleared.

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

`notifications/progress` is supported. A client opts in per request with
`params._meta.progressToken` — a bare key in both eras — and the runtime routes
the provider's reports to that request, ahead of its final result. Providers
never see the token: the host holds it, so progress cannot be addressed at
another request, and reports for a client that asked for none are dropped.

Process providers may originate resource updates and Tools/Resources list changes
through the private `maelys-provider` protocol. These events enter the same filtered,
bounded subscription and Outbox path as native producers.

Prompts, Tasks, Streamable HTTP,
HTTP header routing, and authorization transports are not implemented. Resource *content blocks* returned
by a tool remain distinct from the independently enabled MCP Resources capability.

The upstream conformance runner currently requires an HTTP server and its complete
2026-07-28 requirement set covers capabilities beyond this milestone. Its current
HTTP request/response adapter cannot model a long-lived subscription stream. The repository
therefore runs a documented, pinned subset through a test-only HTTP-to-stdio adapter;
see [Official MCP conformance](official-conformance.md). This validates the supported
wire surface without claiming complete protocol conformance.
