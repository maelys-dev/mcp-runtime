# `@maelys/mcp-provider-sdk`

Dependency-free Node.js SDK for persistent `maelys-provider/5` providers (every version
back to `maelys-provider/3` is still accepted inbound). It owns the protocol loop,
envelopes, structured errors, shutdown and descriptor validation while domain code
supplies tool handlers.

```js
import { completeResult, createProvider, serveProvider } from "@maelys/mcp-provider-sdk";

const provider = createProvider({
  name: "example",
  version: "1.0.0",
  tools: [{
    name: "example.inspect",
    description: "Inspect a resource without mutation.",
    inputSchema: { type: "object", additionalProperties: false },
    outputSchema: { type: "object" },
    effect: "read",
    handler: async () => completeResult({ structuredContent: { ready: true } }),
  }],
});

await serveProvider(provider);
```

The SDK redirects `console.log`, `console.info` and `console.warn` to stderr while
serving. Libraries must still avoid writing directly to `process.stdout`, which is the
private provider protocol channel.

Handlers receive `(arguments, context)`. `context` carries client capabilities and,
on MRTR retries, `inputResponses` and `requestState`. Return `completeResult(...)` or
`inputRequiredResult(...)`; arbitrary JSON results are intentionally rejected.

After host activation, producers and tool handlers may publish events through the
serialized SDK writer:

```js
await provider.events.resourceUpdated("hermes://repository/course.mdx");
await provider.events.resourcesListChanged();
await provider.events.toolsListChanged();
```

Calling an event method before activation or after shutdown fails explicitly.

A tool or resource handler may also open one request back at the client mid-call and
block for the answer - MCP's older multi-round-trip pattern, and the counterpart of
`inputRequiredResult(...)` above rather than a replacement for it:

```js
handler: async (arguments_, context) => {
  const { action, content } = await context.requestElicitation({
    message: "Apply these changes?",
    requestedSchema: { type: "object", properties: { accept: { type: "boolean" } }, required: ["accept"] },
  });
  if (action !== "accept") return completeResult({ content: [{ type: "text", text: "declined" }] });
  return completeResult({ structuredContent: { applied: true } });
},
```

`context.requestSampling(params)` and `context.requestRoots(params)` follow the same
shape for `sampling/createMessage` and `roots/list`. Each returns a promise that
resolves with the client's result or rejects with a `NestedRequestError` carrying a
`code` - `denied`, `timeout`, `cancelled`, `unavailable`, `client_error` (the client's
own JSON-RPC error, on `.data`) or `failed` come from the host; `channel_closed` and
`protocol` are raised locally when no reply can arrive, or the one that did doesn't fit
the wire shape. The host enforces one nested request at a time per call and gates each
method on the client capability the caller declared - a provider that never calls these
helpers behaves exactly as before.
