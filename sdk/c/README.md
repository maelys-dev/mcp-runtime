# C provider SDK

The C provider SDK is the public native helper for writing persistent
`maelys-provider/3` process providers. It keeps application providers from
reimplementing the private JSON Lines protocol by hand.

Use `#include <maelys/mcp/provider_sdk.h>` and link against `libmaelys_mcp.a`.
The SDK:

- builds `provider/describe` from `maelys_mcp_tool_t`, `maelys_mcp_resource_t`
  and `maelys_mcp_resource_template_t` descriptors;
- dispatches `provider/call` and `provider/readResource` into C callbacks;
- serializes complete and `input_required` results;
- gates provider events until `provider/activate` has completed;
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

See `providers/example/main.c` for a complete provider using the SDK.
