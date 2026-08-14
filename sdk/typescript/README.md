# `@maelys/mcp-provider-sdk`

Dependency-free Node.js SDK for persistent `maelys-provider/3` providers. It owns the
protocol loop, envelopes, structured errors, shutdown and descriptor validation while
domain code supplies tool handlers.

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
