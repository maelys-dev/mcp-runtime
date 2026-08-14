# Provider protocol `maelys-provider/3`

External providers are persistent child processes connected through a private
bidirectional socket mapped to stdin and stdout. Messages are UTF-8 JSON Lines.
Provider stdout is protocol-only; diagnostics use stderr. Version 3 retains the
explicit version 2 result model and adds activation plus provider-originated
asynchronous events. Versions 1 and 2 are not accepted.

The host launches an absolute executable directly with `--provider`, without a shell.
Describe, call and shutdown deadlines are independently configurable. Defaults are
5 seconds for discovery, 300 seconds for calls and 2 seconds for shutdown.

## Envelope

```json
{
  "protocol": "maelys-provider/3",
  "id": 1,
  "method": "provider/describe",
  "params": {}
}
```

A response preserves `protocol` and `id` and contains exactly one of `result` or
`error`. A resource handler that does not recognize an URI returns the private error
code `not_found`; the runtime maps it to MCP Invalid Params (`-32602`) without exposing
the provider protocol.

## Activation and event ordering

After discovery and runtime registration, the host sends `provider/activate`. The
provider returns an empty result and must not emit an event before that response. This
barrier is sent only after the runtime Outbox exists, so no startup event is silently
lost.

Requests remain serialized with at most one response outstanding. A dedicated host
reader is nevertheless always active and accepts id-less events between, before or
during normal responses. Unknown methods, malformed event parameters, duplicate JSON
keys, an unexpected response id or any non-v3 envelope fail the provider transport.

The supported event envelopes are:

```json
{"protocol":"maelys-provider/3","method":"provider/notifications/resources/updated","params":{"uri":"hermes://repository/course.mdx"}}
{"protocol":"maelys-provider/3","method":"provider/notifications/resources/list_changed","params":{}}
{"protocol":"maelys-provider/3","method":"provider/notifications/tools/list_changed","params":{}}
```

Resource URIs pass through the same bounded `maelys-uri` normalization as native
events. The runtime then applies negotiated subscription filters, causal coalescence
and Outbox backpressure. Events are notifications: they have no private response.

## `provider/describe`

The result contains `name`, `version`, and the complete `tools` array. It may also
contain `resources` and `resourceTemplates`. Each tool has a
globally unique name, description, object-root `inputSchema`, optional `outputSchema`,
and mandatory `effect`: `read`, `preview`, `apply`, `commit`, or `execute`. Invalid or
unsupported schema definitions fail provider registration.

Static resources declare `uri`, `name`, optional presentation fields, MIME type, and
optional non-negative `size`. Templates declare `uriTemplate` and presentation fields.
Every URI is normalized behind the runtime's opaque URI facade before publication.
Declaring either catalog requires a `provider/readResource` handler.

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

## `provider/readResource`

The request contains a canonical `uri` and the same optional multi-round context as a
tool call. A complete result contains a non-empty `contents` array. Every item repeats
its URI and contains exactly one of UTF-8 `text` or base64 `blob`, plus optional
`mimeType` and `_meta`.

```json
{
  "resultType": "complete",
  "contents": [{
    "uri": "hermes://repository/course.mdx",
    "mimeType": "text/markdown",
    "text": "# Course"
  }]
}
```

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

Version 3 is a private provider ABI, not another public MCP protocol. Older versions
are not accepted; providers and host must upgrade atomically.
