#!/usr/bin/env node
/*
 * End-to-end proof for Phase B (TypeScript SDK) of the nested-MRTR plan: a
 * real fixture provider built on this SDK (nested-fixture-provider.js),
 * completing real provider/nestedRequest <-> provider/nestedReply round
 * trips through the REAL compiled host binary, over actual stdio, with a
 * hand-rolled fake MCP client on the other end - the same shape
 * tests/test_nested_requests.c uses for the host's own suite, just spoken
 * from the client side instead of linking the runtime in-process.
 *
 * Not part of `make check-sdks` (it needs `make` to have built the host
 * first): run it by hand after `make`, e.g.
 *
 *   node sdk/typescript/test/e2e-nested-request.mjs \
 *     build/release/bin/maelys-mcp
 */
import { spawn } from "node:child_process";
import { mkdtempSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "..", "..", "..");
const hostPath = path.resolve(process.argv[2] ?? path.join(repoRoot, "build/release/bin/maelys-mcp"));
const providerPath = path.join(here, "nested-fixture-provider.js");

let failures = 0;
function check(condition, message) {
  if (condition) {
    console.log(`ok - ${message}`);
  } else {
    failures += 1;
    console.error(`NOT OK - ${message}`);
  }
}

const workdir = mkdtempSync(path.join(tmpdir(), "maelys-nested-e2e-"));
const manifestPath = path.join(workdir, "manifest.json");
writeFileSync(manifestPath, JSON.stringify({
  manifestVersion: 1,
  providers: [{ type: "native", path: providerPath }],
  allowEffects: ["apply"],
}));

/* A tiny frame-at-a-time client over the host's real stdout, mirroring
   fake_client_t in tests/test_nested_requests.c: buffer bytes, split on
   newlines, hand back whatever arrives - a nested request and a response
   are both legal next, in either order. */
function createClient(child) {
  let buffer = "";
  const pending = [];
  const waiters = [];
  child.stdout.setEncoding("utf8");
  child.stdout.on("data", (chunk) => {
    buffer += chunk;
    let index;
    while ((index = buffer.indexOf("\n")) >= 0) {
      const line = buffer.slice(0, index);
      buffer = buffer.slice(index + 1);
      if (line.length === 0) continue;
      const message = JSON.parse(line);
      if (waiters.length) waiters.shift()(message);
      else pending.push(message);
    }
  });
  function next(timeoutMs = 10000) {
    if (pending.length) return Promise.resolve(pending.shift());
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        const index = waiters.indexOf(settle);
        if (index >= 0) waiters.splice(index, 1);
        reject(new Error("timed out waiting for a frame from the host"));
      }, timeoutMs);
      const settle = (message) => { clearTimeout(timer); resolve(message); };
      waiters.push(settle);
    });
  }
  async function nextMatching(predicate, timeoutMs = 10000) {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      const remaining = deadline - Date.now();
      if (remaining <= 0) throw new Error("timed out waiting for a matching frame");
      const message = await next(remaining);
      if (predicate(message)) return message;
    }
  }
  function send(message) {
    child.stdin.write(`${JSON.stringify(message)}\n`);
  }
  return { send, next, nextMatching };
}

async function main() {
  const child = spawn(hostPath, ["--manifest", manifestPath], { stdio: ["pipe", "pipe", "pipe"] });
  let stderr = "";
  child.stderr.setEncoding("utf8");
  child.stderr.on("data", (chunk) => { stderr += chunk; });
  const client = createClient(child);

  const isNestedRequest = (message) =>
    typeof message.id === "string" && message.id.startsWith("maelys/nested/") && message.method !== undefined;

  /* --- session #1: declares elicitation and sampling, both round trips happy --- */
  client.send({
    jsonrpc: "2.0", id: 1, method: "initialize",
    params: {
      protocolVersion: "2025-11-25",
      capabilities: { elicitation: {}, sampling: {} },
      clientInfo: { name: "nested-e2e", version: "1" },
    },
  });
  const initResponse = await client.nextMatching((message) => message.id === 1);
  check(!!initResponse.result, "initialize succeeds");
  client.send({ jsonrpc: "2.0", method: "notifications/initialized", params: {} });

  client.send({ jsonrpc: "2.0", id: 2, method: "tools/call", params: { name: "nested.confirm", arguments: {} } });
  const elicitationRequest = await client.nextMatching(isNestedRequest);
  check(elicitationRequest.method === "elicitation/create", "host relays a real elicitation/create request");
  check(elicitationRequest.params?.message === "Apply these changes?", "the provider's elicitation message reaches the client verbatim");
  client.send({ jsonrpc: "2.0", id: elicitationRequest.id, result: { action: "accept", content: { accept: true } } });
  const confirmResponse = await client.nextMatching((message) => message.id === 2);
  const confirmStructured = confirmResponse.result?.structuredContent;
  check(confirmStructured?.action === "accept", "requestElicitation resolves with the client's real answer");
  check(confirmStructured?.content?.accept === true, "the elicitation result content round-trips intact");

  client.send({ jsonrpc: "2.0", id: 3, method: "tools/call", params: { name: "nested.sample", arguments: {} } });
  const samplingRequest = await client.nextMatching(isNestedRequest);
  check(samplingRequest.method === "sampling/createMessage", "host relays a real sampling/createMessage request");
  client.send({
    jsonrpc: "2.0", id: samplingRequest.id,
    result: { role: "assistant", content: { type: "text", text: "pong" }, model: "fixture", stopReason: "endTurn" },
  });
  const sampleResponse = await client.nextMatching((message) => message.id === 3);
  const sampleStructured = sampleResponse.result?.structuredContent;
  check(sampleStructured?.content?.text === "pong", "requestSampling resolves with the client's real answer");

  child.stdin.end();
  const [exitCode] = await new Promise((resolve) => child.on("exit", (code, signal) => resolve([code, signal])));
  check(exitCode === 0, `host exits cleanly after stdin closes (exit code ${exitCode})`);
  check(stderr.trim().length === 0, `host wrote nothing to stderr (got: ${JSON.stringify(stderr)})`);

  /* --- session #2: declares neither capability - the host must refuse the
     nested request itself, before a byte reaches this client. --- */
  const denyChild = spawn(hostPath, ["--manifest", manifestPath], { stdio: ["pipe", "pipe", "pipe"] });
  let denyStderr = "";
  denyChild.stderr.setEncoding("utf8");
  denyChild.stderr.on("data", (chunk) => { denyStderr += chunk; });
  const denyClient = createClient(denyChild);
  denyClient.send({
    jsonrpc: "2.0", id: 1, method: "initialize",
    params: { protocolVersion: "2025-11-25", capabilities: {}, clientInfo: { name: "nested-e2e-deny", version: "1" } },
  });
  await denyClient.nextMatching((message) => message.id === 1);
  denyClient.send({ jsonrpc: "2.0", method: "notifications/initialized", params: {} });
  denyClient.send({ jsonrpc: "2.0", id: 2, method: "tools/call", params: { name: "nested.denied", arguments: {} } });
  const deniedResponse = await denyClient.nextMatching((message) => message.id === 2);
  const deniedStructured = deniedResponse.result?.structuredContent;
  check(deniedStructured?.code === "denied", "a nested request for an undeclared capability comes back denied to the provider");
  check(deniedStructured?.name === "NestedRequestError", "the SDK surfaces the refusal as NestedRequestError");

  denyChild.stdin.end();
  await new Promise((resolve) => denyChild.on("exit", resolve));
  check(denyStderr.trim().length === 0, `deny session wrote nothing to stderr (got: ${JSON.stringify(denyStderr)})`);
}

try {
  await main();
} catch (error) {
  failures += 1;
  console.error("NOT OK - harness threw:", error);
} finally {
  rmSync(workdir, { recursive: true, force: true });
}

if (failures > 0) {
  console.error(`\n${failures} check(s) failed.`);
  process.exit(1);
}
console.log("\nall checks passed.");
