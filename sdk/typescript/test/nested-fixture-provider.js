#!/usr/bin/env node
/*
 * A standalone maelys-provider/5 process, built on this SDK, whose only job
 * is to open real nested requests back at whatever real MCP client the real
 * host relays them to - the end-to-end proof that requestElicitation/
 * requestSampling/requestRoots work through the real host binary over
 * actual stdio, not just against the fake harness in sdk.test.js. See
 * sdk/typescript/test/e2e-nested-request.mjs, which spawns this file via
 * `maelys-mcp --manifest ...` and plays the client side.
 */
import { completeResult, createProvider, serveProvider } from "../src/index.js";

const provider = createProvider({
  name: "nested-fixture",
  version: "1.0.0",
  tools: [
    {
      name: "nested.confirm",
      description: "Ask the client to confirm via a real nested elicitation/create.",
      inputSchema: { type: "object", additionalProperties: false },
      outputSchema: { type: "object" },
      effect: "apply",
      handler: async (_arguments, context) => {
        const result = await context.requestElicitation({
          message: "Apply these changes?",
          requestedSchema: {
            type: "object",
            properties: { accept: { type: "boolean" } },
            required: ["accept"],
          },
        });
        return completeResult({ structuredContent: result });
      },
    },
    {
      name: "nested.sample",
      description: "Ask the client to sample via a real nested sampling/createMessage.",
      inputSchema: { type: "object", additionalProperties: false },
      outputSchema: { type: "object" },
      effect: "read",
      handler: async (_arguments, context) => {
        const result = await context.requestSampling({
          messages: [{ role: "user", content: { type: "text", text: "ping" } }],
          maxTokens: 16,
        });
        return completeResult({ structuredContent: result });
      },
    },
    {
      name: "nested.denied",
      description: "Open a nested request the client never declared the capability for.",
      inputSchema: { type: "object", additionalProperties: false },
      outputSchema: { type: "object" },
      effect: "read",
      handler: async (_arguments, context) => {
        try {
          await context.requestSampling({ messages: [] });
          return completeResult({ structuredContent: { unexpectedSuccess: true } });
        } catch (error) {
          return completeResult({ structuredContent: { code: error.code, name: error.name } });
        }
      },
    },
  ],
});

process.exitCode = await serveProvider(provider);
