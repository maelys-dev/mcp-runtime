import assert from "node:assert/strict";
import test from "node:test";
import { spawn } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const fixturePath = path.join(here, "stdout-garbage-fixture.js");

/*
 * Runs the garbage fixture as a REAL subprocess over plain stdio - not
 * in-process - because the thing under test is a global reassignment of
 * process.stdout.write, and testing it in-process would fight the test
 * runner's own use of the real process.stdout (which is exactly what
 * happened during development of this test: node's reporter serializes
 * test:enqueue/test:dequeue events over the same stdout, and a global patch
 * active for the duration of an in-process serveProvider() call corrupted
 * them). A subprocess keeps the patch's blast radius inside the child, which
 * is also the real deployment shape: a provider's stdout is not shared with
 * anything else.
 */
function runFixture(protectStdout) {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [fixturePath], {
      stdio: ["pipe", "pipe", "pipe"],
      env: { ...process.env, MAELYS_TEST_PROTECT_STDOUT: protectStdout ? "1" : "0" },
    });
    let stdout = Buffer.alloc(0);
    let stderr = Buffer.alloc(0);
    child.stdout.on("data", (chunk) => { stdout = Buffer.concat([stdout, chunk]); });
    child.stderr.on("data", (chunk) => { stderr = Buffer.concat([stderr, chunk]); });
    child.on("error", reject);
    child.on("exit", (code) => resolve({ code, stdout: stdout.toString("utf8"), stderr: stderr.toString("utf8") }));
    const requests = [
      { protocol: "maelys-provider/3", id: 1, method: "provider/describe", params: {} },
      { protocol: "maelys-provider/3", id: 2, method: "provider/call", params: { name: "garbage.emit", arguments: {} } },
      { protocol: "maelys-provider/3", id: 3, method: "provider/shutdown", params: {} },
    ];
    for (const request of requests) child.stdin.write(`${JSON.stringify(request)}\n`);
    child.stdin.end();
  });
}

test("green: protectStdout keeps a direct process.stdout.write off the protocol", async () => {
  const { code, stdout, stderr } = await runFixture(true);
  assert.equal(code, 0);
  const lines = stdout.split("\n").filter((line) => line.length > 0);
  // Every line on the wire is one complete, valid JSON frame - the garbage
  // line never landed here.
  const parsed = lines.map((line) => JSON.parse(line));
  assert.equal(parsed.length, 3);
  assert.deepEqual(parsed.map((message) => message.id), [1, 2, 3]);
  assert.equal(parsed[1].result.structuredContent.wrote, true);
  assert.ok(!stdout.includes("GARBAGE-NOT-JSON"), "garbage must not reach stdout");
  // It went to stderr instead, exactly as the quick fix intends.
  assert.ok(stderr.includes("GARBAGE-NOT-JSON-DIRECT-STDOUT-WRITE"),
    "garbage must be redirected to stderr");
});

test("red: without protectStdout, the same direct write corrupts the protocol stream", async () => {
  const { code, stdout } = await runFixture(false);
  assert.equal(code, 0);
  // This is the vulnerability the quick fix closes, captured as a standing
  // assertion rather than only a step someone ran by hand once: with the
  // fix disabled, the dependency's raw write really does land verbatim in
  // the frame stream, and at least one "line" is therefore not valid JSON.
  assert.ok(stdout.includes("GARBAGE-NOT-JSON-DIRECT-STDOUT-WRITE"),
    "the unprotected run must reproduce the corruption the fix exists for");
  const lines = stdout.split("\n").filter((line) => line.length > 0);
  const bad = lines.filter((line) => {
    try { JSON.parse(line); return false; } catch { return true; }
  });
  assert.ok(bad.length > 0, "at least one wire line must be corrupted without the fix");
});
