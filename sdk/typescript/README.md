# `@maelys/mcp-provider-sdk`

Dependency-free Node.js SDK for persistent `maelys-provider/1` providers. It owns the
protocol loop, envelopes, structured errors, shutdown and descriptor validation while
domain code supplies tool handlers.

```js
import { createProvider, serveProvider } from "@maelys/mcp-provider-sdk";

const provider = createProvider({
  name: "example",
  version: "1.0.0",
  tools: [{
    name: "example.inspect",
    description: "Inspect a resource without mutation.",
    inputSchema: { type: "object", additionalProperties: false },
    outputSchema: { type: "object" },
    effect: "read",
    handler: async () => ({ ready: true }),
  }],
});

await serveProvider(provider);
```

The SDK redirects `console.log`, `console.info` and `console.warn` to stderr while
serving. Libraries must still avoid writing directly to `process.stdout`, which is the
private provider protocol channel.
