# Official MCP conformance

The upstream `@modelcontextprotocol/conformance` server runner accepts only an HTTP
URL. Maelys MCP Runtime intentionally exposes a local stdio transport, so the
repository includes an HTTP-to-stdio adapter used only by tests. It is not installed
and is not a supported runtime transport.

Run the official scenarios that match the current Tools, Resources and MRTR contract,
against both protocol eras:

```sh
make test-mcp-conformance-official
```

The package version is pinned through `MCP_CONFORMANCE_PACKAGE`. Two passes run,
because the two eras have incompatible session models (see
`conformance/run_official_mcp.py`'s module docstring for why):

- **2026-07-28 (modern)**, a fixed, hardcoded list (`MODERN_SCENARIOS`): discovery
  through the official client, every observed message validated against the
  2026-07-28 schema, listing tools and resources, reading text, binary and
  templated resources, enforcing SEP-2164 not-found errors, returning rich tool
  content, and completing the **resumable** MRTR shape (`resultType:
  "input_required"`; the client retries `tools/call` with `inputResponses`) while
  respecting declared client capabilities.
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
also mandate prompts, logging, completion, `ping`, request-scoped SSE and further
scenarios outside mcp-runtime's four modules (tools, MRTR, resources,
subscriptions) or this test adapter's transport (a single-response-per-POST
bridge, not the Streamable HTTP SSE/session semantics `server-sse-multiple-streams`
and `dns-rebinding-protection` need) - see `LEGACY_EXCLUDED` in
`conformance/run_official_mcp.py` for the full, reasoned list. Subscriptions are
implemented in 0.6.0, but the pinned upstream package does not currently expose a
server subscription scenario and this repository's one-response HTTP adapter
cannot model a long-lived listen stream; subscription messages are therefore
validated by native protocol contract tests instead. The runtime does not claim
complete MCP conformance for either era.

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
