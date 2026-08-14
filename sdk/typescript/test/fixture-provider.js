#!/usr/bin/env node
import { completeResult, createProvider, serveProvider } from "../src/index.js";

const provider = createProvider({
  name: "typescript-fixture",
  version: "1.0.0",
  tools: [{
    name: "fixture.echo",
    description: "Echo a message while a dependency writes a diagnostic.",
    inputSchema: {
      type: "object",
      properties: { message: { type: "string", minLength: 1 } },
      required: ["message"],
      additionalProperties: false,
    },
    outputSchema: { type: "object" },
    effect: "read",
    handler: ({ message }) => {
      console.log("simulated third-party diagnostic");
      if (typeof message !== "string") throw new Error("message is required");
      return completeResult({ structuredContent: { message } });
    },
  }],
});

process.exitCode = await serveProvider(provider);
