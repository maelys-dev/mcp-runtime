# Provider protocol `maelys-provider/1`

External providers are persistent child processes connected through a private
bidirectional socket mapped to their stdin and stdout. Messages are UTF-8 JSON Lines.
Provider stdout is reserved exclusively for the protocol; diagnostics use stderr.

The host launches an absolute executable directly with the `--provider` argument.
No shell performs expansion or interpretation.
The reference process adapter applies a five-second socket I/O timeout so a silent
provider cannot block the host indefinitely.

## Envelope

```json
{
  "protocol": "maelys-provider/1",
  "id": 1,
  "method": "provider/describe",
  "params": {}
}
```

A response contains the same `protocol` and `id`, plus either `result` or `error`.

## `provider/describe`

Returns the provider identity and its complete tool catalog:

```json
{
  "name": "hermes",
  "version": "1.0.0",
  "tools": [{
    "name": "hermes.content.preview",
    "title": "Preview an editorial plan",
    "description": "Validates and previews a plan without writing files.",
    "inputSchema": {"type": "object"},
    "outputSchema": {"type": "object"},
    "effect": "preview"
  }]
}
```

Tool names are globally unique. Registration rejects collisions across providers.
It also rejects duplicate names inside one provider description.
`effect` is mandatory and must be `read`, `preview`, `apply`, `commit`, or `execute`.
It is part of the authorization contract, not descriptive documentation.

The runtime accepts only its documented JSON Schema subset and validates every schema
definition before exposing the tool. Unknown keywords fail registration instead of
being advertised and silently ignored.

## `provider/call`

```json
{
  "name": "hermes.content.preview",
  "arguments": {}
}
```

The result may be any JSON value accepted by the declared output schema.

## `provider/shutdown`

The host requests graceful shutdown before closing the socket. Providers should return
an empty result and terminate. The host sends `SIGTERM` if the process remains alive.

## Evolution

Protocol version `1` is intentionally smaller than MCP. It is an internal provider ABI,
not another public tool protocol. Future changes must remain backward-compatible within
the major version or introduce a new version string.
