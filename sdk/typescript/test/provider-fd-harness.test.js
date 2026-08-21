import assert from "node:assert/strict";
import test from "node:test";
import { spawn } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const fixturePath = path.join(here, "fixture-provider.js");

/*
 * A scripted fd-3 harness for MAELYS_PROVIDER_FD (docs/launch-contract-
 * design.md, "The child's descriptors"), without needing a real host or a
 * C helper: Node's own child_process.spawn creates a genuine duplex socket
 * for any 'pipe' stdio slot beyond index 2, exactly the shape the real POSIX
 * launcher hands a native provider on fd 3 under
 * MAELYS_MCP_PROCESS_FD_ISOLATED. fd 0 is 'ignore' (Node's /dev/null) and
 * fd 1 is piped-but-expected-empty, mirroring "stdin has nothing, stdout is
 * not the protocol" without a real host wiring fd 1 to stderr - this harness
 * is testing the SDK's adoption of fd 3, not the launcher's arrangement of
 * fd 0/1, which tests/test_process_launcher.c already covers end to end.
 *
 * fixture-provider.js takes no options - it calls serveProvider(provider)
 * with nothing overridden - so if it adopts MAELYS_PROVIDER_FD at all, it is
 * doing so unprompted by anything test-specific, the same as a real provider
 * binary would.
 */
test("MAELYS_PROVIDER_FD: a real duplex fd carries a full describe/call/shutdown session", async () => {
  const child = spawn(process.execPath, [fixturePath], {
    stdio: ["ignore", "pipe", "pipe", "pipe"],
    env: { ...process.env, MAELYS_PROVIDER_FD: "3" },
  });
  const protocol = child.stdio[3];
  let stdout = Buffer.alloc(0);
  let stderr = Buffer.alloc(0);
  child.stdout.on("data", (chunk) => { stdout = Buffer.concat([stdout, chunk]); });
  child.stderr.on("data", (chunk) => { stderr = Buffer.concat([stderr, chunk]); });

  const frames = [];
  let buffer = "";
  protocol.setEncoding("utf8");
  protocol.on("data", (chunk) => {
    buffer += chunk;
    let index;
    while ((index = buffer.indexOf("\n")) >= 0) {
      const line = buffer.slice(0, index);
      buffer = buffer.slice(index + 1);
      if (line.length > 0) frames.push(JSON.parse(line));
    }
  });

  function next(predicate, timeoutMs = 5000) {
    const deadline = Date.now() + timeoutMs;
    return new Promise((resolve, reject) => {
      const check = () => {
        const index = frames.findIndex(predicate);
        if (index >= 0) return resolve(frames.splice(index, 1)[0]);
        if (Date.now() > deadline) return reject(new Error("timed out waiting for a frame on fd 3"));
        setTimeout(check, 10);
      };
      check();
    });
  }

  const exited = new Promise((resolve) => child.on("exit", (code) => resolve(code)));

  protocol.write(`${JSON.stringify({ protocol: "maelys-provider/3", id: 1, method: "provider/describe", params: {} })}\n`);
  const describeResponse = await next((message) => message.id === 1);
  assert.equal(describeResponse.result.name, "typescript-fixture");

  protocol.write(`${JSON.stringify({
    protocol: "maelys-provider/3", id: 2, method: "provider/call",
    params: { name: "fixture.echo", arguments: { message: "over fd 3" } },
  })}\n`);
  const callResponse = await next((message) => message.id === 2);
  assert.equal(callResponse.result.structuredContent.message, "over fd 3");

  protocol.write(`${JSON.stringify({ protocol: "maelys-provider/3", id: 3, method: "provider/shutdown", params: {} })}\n`);
  await next((message) => message.id === 3);

  const code = await exited;
  assert.equal(code, 0, `provider exited ${code}, stderr: ${stderr.toString("utf8")}`);
  // Nothing on the SDK's own process.stdout: the whole session went over
  // fd 3, and the handler's console.log went to stderr via redirectConsole
  // as always, never to fd 1.
  assert.equal(stdout.length, 0, `unexpected bytes on stdout: ${stdout.toString("utf8")}`);
  assert.ok(stderr.toString("utf8").includes("simulated third-party diagnostic"));
});

test("absent MAELYS_PROVIDER_FD: the same fixture still speaks plain stdio", async () => {
  const child = spawn(process.execPath, [fixturePath], {
    stdio: ["pipe", "pipe", "pipe"],
    env: { ...process.env, MAELYS_PROVIDER_FD: undefined },
  });
  let stdout = Buffer.alloc(0);
  child.stdout.on("data", (chunk) => { stdout = Buffer.concat([stdout, chunk]); });
  child.stdin.write(`${JSON.stringify({ protocol: "maelys-provider/3", id: 1, method: "provider/describe", params: {} })}\n`);
  child.stdin.write(`${JSON.stringify({ protocol: "maelys-provider/3", id: 2, method: "provider/shutdown", params: {} })}\n`);
  child.stdin.end();
  const code = await new Promise((resolve) => child.on("exit", resolve));
  assert.equal(code, 0);
  const lines = stdout.toString("utf8").split("\n").filter((line) => line.length > 0);
  const messages = lines.map((line) => JSON.parse(line));
  assert.deepEqual(messages.map((message) => message.id), [1, 2]);
});
