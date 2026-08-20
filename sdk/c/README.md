# C provider SDK

The C provider SDK is the public native helper for writing persistent
`maelys-provider/5` process providers. It keeps application providers from
reimplementing the private JSON Lines protocol by hand.

Use `#include <maelys/mcp/provider_sdk.h>` and link against `libmaelys_mcp.a`.
The SDK:

- builds `provider/describe` from `maelys_mcp_tool_t`, `maelys_mcp_resource_t`
  and `maelys_mcp_resource_template_t` descriptors;
- dispatches `provider/call` and `provider/readResource` into C callbacks;
- serializes complete and `input_required` results;
- gates provider events until `provider/activate` has completed;
- reports request-scoped progress (`maelys_mcp_provider_sdk_report_progress`);
- opens a request back at the client mid-call and blocks for the reply
  (`maelys_mcp_provider_sdk_request_client`, docs/provider-protocol.md's
  nested requests) - the counterpart of an `input_required` result, not a
  replacement for it: `input_required` ends the call, this keeps it open;
- serializes all protocol writes through one writer mutex;
- isolates stdout by default, including when an options structure only sets a
  message limit; application output from `printf()` is redirected to stderr.
  Set `disable_stdout_isolation` only when the provider intentionally owns its
  stdout transport.

The provider owns its business logic and JSON payload construction. The SDK owns only
the process-provider control plane.

`maelys_mcp_provider_sdk_serve()` borrows its configuration and descriptors for its
full duration. A callback transfers ownership of every JSON value it assigns to its
result; callback error strings must be allocated with `malloc`-compatible storage.
`emit_event()` is safe from provider worker threads after activation. A
provider that starts such threads must supply `shutdown` and join every producer there;
the SDK denies new events once shutdown begins, drains events already writing, then
calls `destroy`.

`maelys_mcp_provider_sdk_request_client()` must be called only from inside a
`call`/`read_resource` callback, on the thread `serve()` is running on: the
serve loop is single threaded, so this is a plain blocking call rather than a
handoff to a second thread, and the host guarantees the correlated
`provider/nestedReply` is the next thing on the wire.

Every session declares `maelys-provider/4` until the provider actually opens
its first nested request; from that request onward it declares `/5`, raised
and never lowered. This is deliberate, not an oversight: announcing `/5`
unconditionally would cost a provider that never nests its compatibility with
a host that predates the version-range fix nested requests shipped with, in
exchange for a capability it never uses. A provider that never calls
`maelys_mcp_provider_sdk_request_client` is therefore byte-for-byte what it
was before this helper existed - there is no opt-in flag to set, and nothing
to configure.

See `providers/example/main.c` for a complete provider using the SDK, and
`tests/helpers/sdk_nested_provider.c` for a minimal one that nests a request.
