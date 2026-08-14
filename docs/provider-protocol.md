# Provider protocol `maelys-provider/2`

External providers are persistent child processes connected through a private
bidirectional socket mapped to stdin and stdout. Messages are UTF-8 JSON Lines.
Provider stdout is protocol-only; diagnostics use stderr. Version 2 intentionally
removes the ambiguous version 1 convention where any JSON value meant a completed
result.

The host launches an absolute executable directly with `--provider`, without a shell.
Describe, call and shutdown deadlines are independently configurable. Defaults are
5 seconds for discovery, 300 seconds for calls and 2 seconds for shutdown.

## Envelope

```json
{
  "protocol": "maelys-provider/2",
  "id": 1,
  "method": "provider/describe",
  "params": {}
}
```

A response preserves `protocol` and `id` and contains exactly one of `result` or
`error`.

## `provider/describe`

The result contains `name`, `version`, and the complete `tools` array. Each tool has a
globally unique name, description, object-root `inputSchema`, optional `outputSchema`,
and mandatory `effect`: `read`, `preview`, `apply`, `commit`, or `execute`. Invalid or
unsupported schema definitions fail provider registration.

## `provider/call`

Initial call:

```json
{
  "name": "hermes.content.apply",
  "arguments": {"plan": "..."},
  "clientCapabilities": {"elicitation": {}}
}
```

A completed provider result is explicit. `content` may mix text, image, audio,
resource-link and embedded-resource blocks; `structuredContent` carries the value
validated against `outputSchema`.

```json
{
  "resultType": "complete",
  "content": [{"type": "text", "text": "Plan applied."}],
  "structuredContent": {"changed": 3}
}
```

For a multi-round request, the provider returns an `input_required` result:

```json
{
  "resultType": "input_required",
  "inputRequests": {
    "confirmation": {
      "method": "elicitation/create",
      "params": {
        "message": "Apply these changes?",
        "requestedSchema": {
          "type": "object",
          "properties": {"accept": {"type": "boolean"}},
          "required": ["accept"]
        }
      }
    }
  },
  "requestState": "opaque-provider-owned-state"
}
```

The client retries `tools/call`; the runtime forwards `inputResponses`, echoed
`requestState`, and `clientCapabilities` to the provider. Providers must authenticate
opaque state against the operation, arguments and expected round before mutating.
The runtime rejects input requests for capabilities the client did not declare with
MCP error `-32021`.

## Content validation

The runtime validates the shape of every content block, MIME family for image/audio,
base64 syntax and the configured message-size bound. Embedded resources require a URI
and exactly one of text or base64 blob. Validation is structural: providers remain
responsible for media authenticity, malware scanning, URI policy and domain semantics.

## Conformance and shutdown

`conformance/provider_conformance.py` checks the persistent lifecycle, envelopes,
ids, errors, schemas, explicit result variants, declared call cases, graceful
shutdown and stdout isolation. `provider/shutdown` returns `{}`; a process that does
not terminate within its deadline receives `SIGTERM`, then `SIGKILL` after one final
bounded grace period.

Version 2 is a private provider ABI, not another public MCP protocol. Version 1 is not
accepted by the 0.3 runtime; providers must upgrade atomically with the host.
