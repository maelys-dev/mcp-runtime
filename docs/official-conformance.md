# Official MCP conformance

The upstream `@modelcontextprotocol/conformance` server runner accepts only an HTTP
URL, and since H4 of the HTTP transport that is exactly what it gets for the modern
era: `conformance/run_official_mcp.py` starts the real `maelys-mcp` binary with
`--http-listen` and points the official runner straight at its socket, over the
actual Streamable HTTP transport this binary ships - no adapter in the path. The
2025-11-25 (legacy) era has no such listener to point at (the HTTP transport serves
2026-07-28 ONLY - `maelys-mcp --help`), so that pass still runs through
`StdioBridge`, a test adapter that forwards one JSON-RPC object per POST to a
stdio-hosted runtime and returns its response. It is not installed and is not a
supported runtime transport; it exists purely so the legacy pass has an HTTP URL
to hand the official runner at all.

Run the official scenarios that match the current Tools, Resources and MRTR contract,
against both protocol eras:

```sh
make test-mcp-conformance-official
```

The package version is pinned through `MCP_CONFORMANCE_PACKAGE`. Two passes run,
because the two eras have incompatible session models AND, as of H4, different
transports (see `conformance/run_official_mcp.py`'s module docstring for the full
reasoning):

- **2026-07-28 (modern)**, a fixed, hardcoded list (`MODERN_SCENARIOS`), run
  directly against the real HTTP listener: discovery through the official client,
  every observed message validated against the 2026-07-28 schema, listing tools
  and resources, reading text, binary and templated resources, enforcing SEP-2164
  not-found errors, returning rich tool content, completing the **resumable** MRTR
  shape (`resultType: "input_required"`; the client retries `tools/call` with
  `inputResponses`) while respecting declared client capabilities, serving
  concurrent POST streams (`server-sse-multiple-streams`), and rejecting
  non-loopback `Host`/`Origin` headers (`dns-rebinding-protection`) - the last two
  are new to this pass in H4: the bridge they used to be excluded from could
  implement neither SSE transport semantics nor Host/Origin validation, and the
  real listener implements both.
- **2025-11-25 (legacy)**, the frozen server requirement set fetched live from the
  official CLI (`legacy_scenarios()`), minus `LEGACY_EXCLUDED` - a scenario upstream
  adds, renames or drops is picked up (or stops applying) automatically; only
  mcp-runtime's own scope boundary needs a human to update, and even that is
  checked live rather than left to rot as a comment (see
  `check_exclusions_still_hold()`). This pass additionally completes the
  **nested** MRTR shape: the server opens a real `sampling/createMessage` or
  `elicitation/create` request mid-`tools/call`, over the same connection, and
  blocks for the client's reply - `tools-call-sampling`, `tools-call-elicitation`,
  `elicitation-sep1034-defaults` and `elicitation-sep1330-enums`, served by
  `conformance/official_tools_provider.py`'s `test_sampling`/`test_elicitation`/
  `test_elicitation_sep1034_defaults`/`test_elicitation_sep1330_enums` handlers via
  the Python SDK's `context.request_sampling`/`context.request_elicitation`. See
  `docs/architecture.md`'s "Nested requests" section for why mcp-runtime needs both
  MRTR shapes rather than just the resumable one 2026-07-28 tests.

This is deliberately a partial conformance result. The frozen requirement sets
also mandate prompts, logging, completion, `ping`, and further scenarios outside
mcp-runtime's four modules (tools, MRTR, resources, subscriptions), or - for the
legacy pass only - outside what `StdioBridge`'s single-response-per-POST transport
can model (`server-sse-multiple-streams`, `dns-rebinding-protection`; the modern
pass runs both directly against the real listener instead, see above) - see
`LEGACY_EXCLUDED` in `conformance/run_official_mcp.py` for the full, reasoned list.
Subscriptions are implemented in 0.6.0, but the pinned upstream package does not
currently expose a server subscription scenario; subscription messages are
therefore validated by native protocol contract tests instead. The runtime does
not claim complete MCP conformance for either era.

The private `maelys-provider` protocol has a separate black-box runner in
`conformance/provider_conformance.py`, which opens at the version 3 floor and
accepts either version back, exactly as the host does. The TypeScript and Python
packages under `sdk/` are provider SDKs only; they are not general MCP client or
server SDKs. This runner also plays the host's side of the nested round trip
(`provider/nestedRequest` / `provider/nestedReply`) for a case that declares one,
so it exercises the same nested-request path against each SDK's own nested
fixture provider (`tests/helpers/sdk_nested_provider.c`,
`sdk/typescript/test/nested-fixture-provider.js`,
`sdk/python/tests/nested_fixture_provider.py`) without needing a real host
process or client:

```sh
make test-provider-conformance
```
