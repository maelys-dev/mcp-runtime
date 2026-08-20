# Provider protocol `maelys-provider/5`

External providers are persistent child processes connected through a private
bidirectional socket mapped to stdin and stdout. Messages are UTF-8 JSON Lines.
Provider stdout is protocol-only; diagnostics use stderr. Version 3 retained the
explicit version 2 result model and added activation plus provider-originated
asynchronous events. Version 4 adds request-scoped progress. Version 5 adds
nested requests: a provider may open a request back at the *client* in the
middle of a call and block for its answer. Versions 1 and 2 are not accepted.

## Version negotiation

The version is negotiated, not fixed. Every SDK compares the protocol string
exactly, so a host that simply began sending a newer one would break every
existing provider on every request. Instead the host opens at the floor,
`maelys-provider/3`, reads the version the provider declares in its own frames -
providers state theirs rather than echoing the host's - and speaks that version
from then on. The version is raised, never lowered, and is learned from any
frame the provider sends, not only from its responses: a provider announces /5
on the nested request itself.

The host accepts **every** version from the floor to the current one, not just
those two. A version 3 or version 4 provider therefore keeps working untouched
and is never sent anything it would reject; only a version 4 provider may send
progress, and only a version 5 provider may open a nested request. This mirrors
how MCP's own dated revisions are negotiated rather than matched
(`is_supported_legacy_version`).

Native providers should use the public C provider SDK
(`maelys/mcp/provider_sdk.h`) rather than writing this protocol loop by hand. The
wire format below remains the contract between the host and SDK implementations.

The host launches an absolute executable directly with `--provider`, without a shell.
Describe, call and shutdown deadlines are independently configurable. Defaults are
5 seconds for discovery, 300 seconds for calls and 2 seconds for shutdown.

## Envelope

```json
{
  "protocol": "maelys-provider/5",
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

Requests remain serialized with at most one response outstanding, in both
directions. A dedicated host reader is nevertheless always active and accepts
id-less frames between, before or during normal responses. It sorts them into
three kinds: a response (`id` present), a nested request (`method` present, no
`id`, `provider/nestedRequest`), and the whitelisted asynchronous events below.
Unknown methods, malformed event parameters, duplicate JSON keys, an unexpected
response id or an envelope whose version is not supported fail the provider
transport.

The supported event envelopes are:

```json
{"protocol":"maelys-provider/4","method":"provider/notifications/resources/updated","params":{"uri":"hermes://repository/course.mdx"}}
{"protocol":"maelys-provider/4","method":"provider/notifications/resources/list_changed","params":{}}
{"protocol":"maelys-provider/4","method":"provider/notifications/tools/list_changed","params":{}}
```

Resource URIs pass through the same bounded `maelys-uri` normalization as native
events. The runtime then applies negotiated subscription filters, causal coalescence
and Outbox backpressure. Events are notifications: they have no private response.

## Progress (version 4)

A provider may report progress on the call it is currently handling:

```json
{"protocol":"maelys-provider/4","method":"provider/notifications/progress","params":{"progress":50,"total":100,"message":"halfway"}}
```

`progress` is required; `total` and `message` are optional. This frame differs
from the events above in two ways that matter:

- **It is request-scoped, not fanned out.** The host routes it to the one call
  in flight rather than to every subscriber. That is unambiguous because
  provider calls are strictly single-outstanding, so no correlation table is
  needed - which is also why it is only valid while a call is being handled.
- **It carries no progress token.** The host holds the token the client
  supplied and fills it in. A provider therefore cannot address progress at a
  request that is not its own, and cannot forge one for a client that asked for
  none - such frames are simply dropped.

The host never emits these from its reader thread. They are queued and drained
by the thread handling the call, so every emission passes through that
request's response sink in order, ahead of its final result, rather than around
it.

## Nested requests (version 5)

A provider handling a call may open one request back at the client and block
for the answer. This is MCP's older multi-round-trip pattern - the one a
schema-validating `2025-11-25` client understands - and the counterpart of the
`input_required` result below, not a replacement for it: `input_required` ends
the call and lets the client retry, while a nested request keeps the call open.

```json
{"protocol":"maelys-provider/5","method":"provider/nestedRequest","params":{"nestedId":"n1","method":"elicitation/create","params":{"message":"Apply these changes?"}}}
```

The host answers with a correlated reply on the same connection:

```json
{"protocol":"maelys-provider/5","method":"provider/nestedReply","params":{"nestedId":"n1","result":{"action":"accept","content":{"ok":true}}}}
```

```json
{"protocol":"maelys-provider/5","method":"provider/nestedReply","params":{"nestedId":"n1","error":{"code":"timeout","message":"nested request deadline exceeded"}}}
```

Neither frame carries a top-level `id`. That is the collision-avoidance
mechanism: the call-scoped `nestedId` the provider chooses lives in a different
namespace from the host's request ids, so no id-space negotiation is needed in
either direction. `nestedId` is echoed verbatim.

Rules the host enforces:

- **`method` must be one of `elicitation/create`, `sampling/createMessage` or
  `roots/list`,** and the client must have declared the matching capability.
  Anything else is refused with `denied` before a byte reaches the client - the
  same set, and the same check, the `input_required` path applies, because
  which client surfaces a provider may reach must not depend on which MRTR
  shape it picked.
- **One at a time.** A nested request while another is outstanding, or one sent
  while no call is being handled, fails the provider transport. The wire is
  strictly single-outstanding in both directions, and a provider that broke
  that has lost track of which reply answers which request.
- **The call deadline is suspended, not raced.** The provider call deadline
  (300 seconds by default) is extended by however long the client took, because
  the thing answering an elicitation may be a person. The nested request has
  its own, separate deadline, ten minutes by default.
- **Errors are reported, not hidden.** `code` is one of `client_error` (the
  client answered with a JSON-RPC error, which travels on as `data`), `denied`,
  `timeout`, `cancelled` (the client cancelled the outer call, or the
  connection went away), `unavailable` (this call cannot nest) or `failed`.

The host never performs the client round trip on its reader thread: the frame
is handed to the thread inside the call, which sends it through that request's
own response sink - so it stays ordered ahead of the final result and stays
visible to a `wrap_sink` middleware - and writes the reply back over the
exchange it already owns.

One consequence follows from the wire being single-outstanding and is worth
stating rather than discovering: a call waiting on a nested request keeps the
provider's exchange, so another call to *that provider* waits behind it for as
long as the client takes. The connection itself keeps being read throughout -
cancellations, other providers' calls and this call's own reply all get
through - but a provider that opens long human-in-the-loop requests serialises
its own work by design.

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

Version 5 is a private provider ABI, not another public MCP protocol. Versions 1
and 2 are not accepted. Versions 3 and 4 are, and keep working unchanged - only
a provider that wants progress or nested requests needs to declare a newer one.
