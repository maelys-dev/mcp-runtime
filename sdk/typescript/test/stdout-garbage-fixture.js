#!/usr/bin/env node
/*
 * Fixture for the TypeScript quick fix (docs/launch-contract-design.md, "The
 * TypeScript quick fix, and its limits"): a handler that bypasses
 * console.log entirely and writes straight to process.stdout.write, exactly
 * the third-party-dependency hole the quick fix targets. Real stdio, no
 * MAELYS_PROVIDER_FD - this proves the fix in the STDIO layout, where fd 1
 * genuinely is the protocol and there is something for it to protect.
 *
 * MAELYS_TEST_PROTECT_STDOUT=0 disables the fix (serveProvider's
 * protectStdout: false), so the same fixture also produces the "red" half of
 * the red/green proof: run once each way and diff what lands on stdout.
 */
import { completeResult, createProvider, serveProvider } from "../src/index.js";

const provider = createProvider({
  name: "stdout-garbage-fixture",
  version: "1.0.0",
  tools: [{
    name: "garbage.emit",
    description: "Write raw garbage to fd 1 mid-call, bypassing console.log.",
    inputSchema: { type: "object", additionalProperties: false },
    outputSchema: { type: "object" },
    effect: "read",
    handler: () => {
      process.stdout.write("GARBAGE-NOT-JSON-DIRECT-STDOUT-WRITE\n");
      return completeResult({ structuredContent: { wrote: true } });
    },
  }],
});

const protectStdout = process.env.MAELYS_TEST_PROTECT_STDOUT !== "0";
process.exitCode = await serveProvider(provider, { protectStdout });
