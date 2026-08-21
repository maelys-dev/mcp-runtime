import assert from "node:assert/strict";
import test from "node:test";
import { Readable } from "node:stream";
import { NestedRequestError, PROTOCOL, completeResult, createProvider, describeProvider, handleProviderMessage, inputRequiredResult, resourceResult, serveProvider, validateSchemaDefinition } from "../src/index.js";

/*
 * A minimal fake host for the nested-request tests below: `input` is what
 * serveProvider reads, `output` is every frame it wrote, and `onNestedRequest`
 * fires the moment a provider/nestedRequest is actually written (not before -
 * the test must react to the real frame, the same way a real host would),
 * so a reply can be pushed back at exactly the point a live host would send
 * one. Never call `end()` before every scripted send has happened: a
 * Readable rejects pushes after its EOF sentinel.
 */
function createHarness({ onNestedRequest } = {}) {
  const input = new Readable({ read() {} });
  const output = [];
  function send(message) { input.push(`${JSON.stringify(message)}\n`); }
  function end() { input.push(null); }
  const writeLine = (line) => {
    const message = JSON.parse(line);
    output.push(message);
    if (message.method === "provider/nestedRequest" && onNestedRequest) {
      onNestedRequest(message.params, { send, end });
    }
  };
  return { input, output, send, end, writeLine };
}

function nestingProvider(name, invoke) {
  return createProvider({
    name, version: "1",
    tools: [{
      name: "op", description: "Op", inputSchema: { type: "object" }, effect: "apply",
      handler: async (_arguments, context) => completeResult({ structuredContent: await invoke(context) }),
    }],
  });
}

function fixtureProvider() {
  return createProvider({
    name: "fixture",
    version: "1.0.0",
    tools: [{
      name: "fixture.echo",
      description: "Echo a message.",
      inputSchema: {
        type: "object",
        properties: { message: { type: "string", minLength: 1 } },
        required: ["message"],
        additionalProperties: false,
      },
      outputSchema: { type: "object" },
      effect: "read",
      handler: async ({ message }) => completeResult({ structuredContent: { message } }),
    }],
  });
}

test("description never exposes handlers", () => {
  const description = describeProvider(fixtureProvider());
  assert.equal(description.name, "fixture");
  assert.equal(description.tools[0].handler, undefined);
  assert.equal(description.tools[0].effect, "read");
});

test("persistent messages preserve protocol and integer ids", async () => {
  const provider = fixtureProvider();
  const response = await handleProviderMessage(provider, {
    protocol: "maelys-provider/3",
    id: 7,
    method: "provider/call",
    params: { name: "fixture.echo", arguments: { message: "hello" } },
  });
  // Spelled out rather than compared against PROTOCOL_DECLARED: the property
  // under test is that a call which never nests declares the older version -
  // a constant reference would move with the code under its own mutation.
  assert.deepEqual(response, { protocol: "maelys-provider/4", id: 7, result: {
    resultType: "complete", structuredContent: { message: "hello" },
  } });
  await assert.rejects(() => handleProviderMessage(provider, {
    protocol: "wrong",
    id: 8,
    method: "provider/describe",
    params: {},
  }), /unsupported provider protocol/);
});

test("unknown tools fail without invoking a handler", async () => {
  await assert.rejects(() => handleProviderMessage(fixtureProvider(), {
    protocol: "maelys-provider/3",
    id: 9,
    method: "provider/call",
    params: { name: "fixture.missing", arguments: {} },
  }), /unknown provider tool/);
});

test("MRTR forwards retry context and emits an explicit input_required result", async () => {
  let seen;
  const provider = createProvider({
    name: "mrtr", version: "1", tools: [{
      name: "confirm", description: "Confirm", inputSchema: { type: "object" },
      effect: "apply",
      handler: (_arguments, context) => {
        seen = context;
        return context.inputResponses ? completeResult({ content: [{ type: "text", text: "confirmed" }] }) :
          inputRequiredResult({ inputRequests: { confirm: {
            method: "elicitation/create", params: { message: "Confirm?", requestedSchema: { type: "object" } },
          } }, requestState: "opaque" });
      },
    }],
  });
  const first = await handleProviderMessage(provider, {
    protocol: "maelys-provider/3", id: 1, method: "provider/call",
    params: { name: "confirm", arguments: {} },
  });
  assert.equal(first.result.resultType, "input_required");
  const second = await handleProviderMessage(provider, {
    protocol: "maelys-provider/3", id: 2, method: "provider/call",
    params: { name: "confirm", arguments: {}, inputResponses: { confirm: { action: "accept" } }, requestState: "opaque" },
  });
  assert.equal(second.result.resultType, "complete");
  assert.equal(seen.inputResponses.confirm.action, "accept");
  assert.equal(seen.requestState, "opaque");
  assert.equal(seen.clientCapabilities, undefined);
  assert.equal(typeof seen.requestElicitation, "function");
  assert.equal(typeof seen.requestSampling, "function");
  assert.equal(typeof seen.requestRoots, "function");
});

test("schema validation rejects unsupported keywords and drift-prone required fields", () => {
  assert.throws(() => validateSchemaDefinition({ type: "object", oneOf: [] }), /unsupported key/);
  assert.throws(() => validateSchemaDefinition({ type: "object", required: ["missing"] }), /undeclared property/);
  assert.throws(() => validateSchemaDefinition({ type: "integer", minLength: 1 }), /length keywords/);
  assert.throws(() => validateSchemaDefinition({ type: "string", minimum: 1 }), /numeric bounds/);
  assert.throws(() => validateSchemaDefinition({ type: "string", enum: [1] }), /does not match/);
  assert.throws(() => createProvider({
    name: "duplicate",
    version: "1",
    tools: [fixtureProvider().tools[0], fixtureProvider().tools[0]],
  }), /duplicate provider tool/);
});

test("resources are described and read through the private provider contract", async () => {
  const provider = createProvider({
    name: "resources",
    version: "1",
    resources: [{ uri: "fixture://about", name: "About", mimeType: "text/plain" }],
    resourceTemplates: [{ uriTemplate: "fixture://echo/{value}", name: "Echo" }],
    readResource: async (uri) => resourceResult([{ uri, mimeType: "text/plain", text: `read ${uri}` }]),
  });
  const description = describeProvider(provider);
  assert.deepEqual(description.resources, [{ uri: "fixture://about", name: "About", mimeType: "text/plain" }]);
  assert.equal(description.resources[0].size, undefined);
  assert.equal(description.resourceTemplates[0].uriTemplate, "fixture://echo/{value}");
  const response = await handleProviderMessage(provider, {
    protocol: "maelys-provider/3", id: 10, method: "provider/readResource",
    params: { uri: "fixture://echo/hello" },
  });
  assert.deepEqual(response.result, { resultType: "complete", contents: [{
    uri: "fixture://echo/hello", mimeType: "text/plain", text: "read fixture://echo/hello",
  }] });
});

test("provider/3 serializes asynchronous events with normal responses", async () => {
  let provider;
  provider = createProvider({
    name: "events", version: "1", tools: [{
      name: "events.emit", description: "Emit all event shapes.",
      inputSchema: { type: "object" }, effect: "execute",
      handler: async () => {
        await provider.events.resourceUpdated("fixture://course/one");
        await provider.events.resourcesListChanged();
        await provider.events.toolsListChanged();
        return completeResult({ structuredContent: { emitted: 3 } });
      },
    }],
  });
  await assert.rejects(() => provider.events.toolsListChanged(), /before activation/);
  const requests = [
    { protocol: "maelys-provider/3", id: 1, method: "provider/activate", params: {} },
    { protocol: "maelys-provider/3", id: 2, method: "provider/call", params: { name: "events.emit", arguments: {} } },
    { protocol: "maelys-provider/3", id: 3, method: "provider/shutdown", params: {} },
  ];
  const output = [];
  await serveProvider(provider, {
    input: Readable.from(requests.map((request) => `${JSON.stringify(request)}\n`)),
    writeLine: (line) => { output.push(JSON.parse(line)); },
    redirectConsole: false,
    protectStdout: false,
  });
  assert.deepEqual(output.map((message) => message.method ?? `response:${message.id}`), [
    "response:1",
    "provider/notifications/resources/updated",
    "provider/notifications/resources/list_changed",
    "provider/notifications/tools/list_changed",
    "response:2",
    "response:3",
  ]);
  assert.equal(output[1].params.uri, "fixture://course/one");
  await assert.rejects(() => provider.events.toolsListChanged(), /after shutdown/);
});

test("handleProviderMessage accepts every protocol from the floor to the current version", async () => {
  const provider = nestingProvider("range", async () => ({ ok: true }));
  for (const protocol of ["maelys-provider/3", "maelys-provider/4", "maelys-provider/5"]) {
    const response = await handleProviderMessage(provider, {
      protocol, id: 1, method: "provider/describe", params: {},
    });
    // The host addresses a provider at whatever it last declared - not
    // necessarily the newest version this SDK speaks - so /4 has to be
    // accepted even though it is neither the floor nor PROTOCOL.
    assert.equal(response.protocol, "maelys-provider/4");
  }
  await assert.rejects(() => handleProviderMessage(provider, {
    protocol: "maelys-provider/2", id: 1, method: "provider/describe", params: {},
  }), /unsupported provider protocol/);
});

test("a provider that never nests declares maelys-provider/4 on every frame it writes", async () => {
  let provider;
  provider = createProvider({
    name: "never-nests", version: "1", tools: [{
      name: "echo", description: "Echo without ever calling a nested helper.",
      inputSchema: { type: "object" }, effect: "read",
      handler: async () => {
        await provider.events.resourcesListChanged();
        return completeResult({ structuredContent: { ok: true } });
      },
    }],
  });
  const requests = [
    { protocol: "maelys-provider/3", id: 1, method: "provider/describe", params: {} },
    { protocol: "maelys-provider/3", id: 2, method: "provider/activate", params: {} },
    { protocol: "maelys-provider/3", id: 3, method: "provider/call", params: { name: "echo", arguments: {} } },
    { protocol: "maelys-provider/3", id: 4, method: "provider/shutdown", params: {} },
  ];
  const output = [];
  await serveProvider(provider, {
    input: Readable.from(requests.map((request) => `${JSON.stringify(request)}\n`)),
    writeLine: (line) => { output.push(JSON.parse(line)); },
    redirectConsole: false,
    protectStdout: false,
  });
  // Every response and every event: describe, activate, the call's own
  // response, the event the handler emitted mid-call, and shutdown.
  assert.equal(output.length, 5);
  // Spelled out, not compared against PROTOCOL_DECLARED: the property under
  // test is that these bytes are unchanged from before this SDK could nest,
  // and a constant reference would move with the code under its own
  // mutation rather than pin the literal.
  for (const message of output) assert.equal(message.protocol, "maelys-provider/4");
});

test("the first nested request raises the declared protocol to /5 for the rest of the session, never lowered", async () => {
  const provider = createProvider({
    name: "raises-once", version: "1",
    tools: [
      {
        name: "confirm", description: "The one tool that nests.",
        inputSchema: { type: "object" }, effect: "apply",
        handler: async (_arguments, context) => completeResult({
          structuredContent: await context.requestElicitation({ message: "Apply?", requestedSchema: { type: "object" } }),
        }),
      },
      {
        name: "echo", description: "An ordinary tool called after the nest resolves.",
        inputSchema: { type: "object" }, effect: "read",
        handler: async () => completeResult({ structuredContent: { ok: true } }),
      },
    ],
  });
  const harness = createHarness({
    onNestedRequest: (params, { send, end }) => {
      send({ protocol: PROTOCOL, method: "provider/nestedReply",
        params: { nestedId: params.nestedId, result: { action: "accept" } } });
      // A second, ordinary call after the nest resolves - proves the raise
      // holds for the rest of the session, not just for the one frame that
      // announced it.
      send({ protocol: PROTOCOL, id: 2, method: "provider/call", params: { name: "echo", arguments: {} } });
      send({ protocol: PROTOCOL, id: 3, method: "provider/shutdown", params: {} });
      end();
    },
  });
  // Answered before the nesting call is even dispatched (the dispatch loop
  // is strictly one frame at a time), so this pins the "before" state too.
  harness.send({ protocol: "maelys-provider/3", id: 1, method: "provider/describe", params: {} });
  harness.send({ protocol: "maelys-provider/3", id: 100, method: "provider/call", params: { name: "confirm", arguments: {} } });
  await serveProvider(provider, { input: harness.input, writeLine: harness.writeLine, redirectConsole: false, protectStdout: false });

  assert.equal(harness.output.find((message) => message.id === 1).protocol, "maelys-provider/4");
  const nestedRequestFrame = harness.output.find((message) => message.method === "provider/nestedRequest");
  assert.equal(nestedRequestFrame.protocol, "maelys-provider/5");
  // The nesting call's own response - not just what comes after it.
  assert.equal(harness.output.find((message) => message.id === 100).protocol, "maelys-provider/5");
  // An unrelated call dispatched afterward: still /5, not reverted to /4.
  assert.equal(harness.output.find((message) => message.id === 2).protocol, "maelys-provider/5");
  assert.equal(harness.output.find((message) => message.id === 3).protocol, "maelys-provider/5");
});

test("requestElicitation/requestSampling/requestRoots reject outside serveProvider", async () => {
  for (const method of ["requestElicitation", "requestSampling", "requestRoots"]) {
    const provider = nestingProvider("standalone", (context) => context[method]({}));
    await assert.rejects(() => handleProviderMessage(provider, {
      protocol: "maelys-provider/5", id: 1, method: "provider/call", params: { name: "op", arguments: {} },
    }), (error) => {
      assert.ok(error instanceof NestedRequestError);
      assert.equal(error.code, "unavailable");
      assert.match(error.message, /unavailable outside serveProvider/);
      return true;
    });
  }
});

test("requestElicitation completes a real round trip through serveProvider", async () => {
  const provider = nestingProvider("elicit", (context) => context.requestElicitation({
    message: "Apply these changes?",
    requestedSchema: { type: "object", properties: { accept: { type: "boolean" } }, required: ["accept"] },
  }));
  const harness = createHarness({
    onNestedRequest: (params, { send, end }) => {
      assert.equal(params.method, "elicitation/create");
      assert.deepEqual(params.params, {
        message: "Apply these changes?",
        requestedSchema: { type: "object", properties: { accept: { type: "boolean" } }, required: ["accept"] },
      });
      send({ protocol: PROTOCOL, method: "provider/nestedReply",
        params: { nestedId: params.nestedId, result: { action: "accept", content: { ok: true } } } });
      send({ protocol: PROTOCOL, id: 2, method: "provider/shutdown", params: {} });
      end();
    },
  });
  harness.send({ protocol: "maelys-provider/3", id: 1, method: "provider/call", params: { name: "op", arguments: {} } });
  await serveProvider(provider, { input: harness.input, writeLine: harness.writeLine, redirectConsole: false, protectStdout: false });
  assert.deepEqual(harness.output.find((message) => message.id === 1).result.structuredContent,
    { action: "accept", content: { ok: true } });
});

test("requestSampling completes a real round trip through serveProvider", async () => {
  const provider = nestingProvider("sample", (context) => context.requestSampling({
    messages: [{ role: "user", content: { type: "text", text: "hi" } }],
  }));
  const harness = createHarness({
    onNestedRequest: (params, { send, end }) => {
      assert.equal(params.method, "sampling/createMessage");
      send({ protocol: PROTOCOL, method: "provider/nestedReply",
        params: { nestedId: params.nestedId, result: { role: "assistant", content: { type: "text", text: "hello" } } } });
      send({ protocol: PROTOCOL, id: 2, method: "provider/shutdown", params: {} });
      end();
    },
  });
  harness.send({ protocol: "maelys-provider/3", id: 1, method: "provider/call", params: { name: "op", arguments: {} } });
  await serveProvider(provider, { input: harness.input, writeLine: harness.writeLine, redirectConsole: false, protectStdout: false });
  assert.deepEqual(harness.output.find((message) => message.id === 1).result.structuredContent,
    { role: "assistant", content: { type: "text", text: "hello" } });
});

test("requestRoots completes a real round trip through serveProvider", async () => {
  const provider = nestingProvider("roots", async (context) => {
    const { roots } = await context.requestRoots();
    return { count: roots.length };
  });
  const harness = createHarness({
    onNestedRequest: (params, { send, end }) => {
      assert.equal(params.method, "roots/list");
      assert.deepEqual(params.params, {});
      send({ protocol: PROTOCOL, method: "provider/nestedReply",
        params: { nestedId: params.nestedId, result: { roots: [{ uri: "file:///a" }, { uri: "file:///b" }] } } });
      send({ protocol: PROTOCOL, id: 2, method: "provider/shutdown", params: {} });
      end();
    },
  });
  harness.send({ protocol: "maelys-provider/3", id: 1, method: "provider/call", params: { name: "op", arguments: {} } });
  await serveProvider(provider, { input: harness.input, writeLine: harness.writeLine, redirectConsole: false, protectStdout: false });
  assert.deepEqual(harness.output.find((message) => message.id === 1).result.structuredContent, { count: 2 });
});

test("a host denial rejects the nested-request promise with the host's code", async () => {
  const provider = nestingProvider("denied", async (context) => {
    try {
      await context.requestElicitation({ message: "Apply?", requestedSchema: { type: "object" } });
      throw new Error("expected the nested request to be denied");
    } catch (error) {
      assert.ok(error instanceof NestedRequestError);
      return { code: error.code, message: error.message };
    }
  });
  const harness = createHarness({
    onNestedRequest: (params, { send, end }) => {
      send({ protocol: PROTOCOL, method: "provider/nestedReply",
        params: { nestedId: params.nestedId, error: { code: "denied", message: "client did not declare elicitation" } } });
      send({ protocol: PROTOCOL, id: 2, method: "provider/shutdown", params: {} });
      end();
    },
  });
  harness.send({ protocol: "maelys-provider/3", id: 1, method: "provider/call", params: { name: "op", arguments: {} } });
  await serveProvider(provider, { input: harness.input, writeLine: harness.writeLine, redirectConsole: false, protectStdout: false });
  assert.deepEqual(harness.output.find((message) => message.id === 1).result.structuredContent,
    { code: "denied", message: "client did not declare elicitation" });
});

test("a nested reply queued alongside ordinary dispatch traffic loses nothing and reorders nothing", async () => {
  const provider = createProvider({
    name: "interleaved", version: "1",
    tools: [
      {
        name: "confirm", description: "Confirm", inputSchema: { type: "object" }, effect: "apply",
        handler: async (_arguments, context) => completeResult({
          structuredContent: await context.requestElicitation({ message: "Apply?", requestedSchema: { type: "object" } }),
        }),
      },
      {
        name: "echo", description: "Echo", inputSchema: { type: "object" }, effect: "read",
        handler: async () => completeResult({ structuredContent: { echoed: true } }),
      },
    ],
  });
  const harness = createHarness({
    onNestedRequest: (params, { send, end }) => {
      /* The reply and an unrelated ordinary call arrive in a single write -
         one chunk, two lines - so the pump must split and route both
         correctly: the reply to the waiting handler, the call onto the
         dispatch queue, in arrival order, without either going missing. */
      harness.input.push(
        `${JSON.stringify({ protocol: PROTOCOL, method: "provider/nestedReply",
          params: { nestedId: params.nestedId, result: { action: "accept" } } })}\n` +
        `${JSON.stringify({ protocol: PROTOCOL, id: 2, method: "provider/call",
          params: { name: "echo", arguments: {} } })}\n`,
      );
      send({ protocol: PROTOCOL, id: 3, method: "provider/shutdown", params: {} });
      end();
    },
  });
  harness.send({ protocol: "maelys-provider/3", id: 1, method: "provider/call", params: { name: "confirm", arguments: {} } });
  await serveProvider(provider, { input: harness.input, writeLine: harness.writeLine, redirectConsole: false, protectStdout: false });
  const ids = harness.output.filter((message) => typeof message.id === "number").map((message) => message.id);
  assert.deepEqual(ids, [1, 2, 3]);
  assert.deepEqual(harness.output.find((message) => message.id === 1).result.structuredContent, { action: "accept" });
  assert.deepEqual(harness.output.find((message) => message.id === 2).result.structuredContent, { echoed: true });
});

test("a malformed nested reply rejects with a locally synthesized protocol error", async () => {
  const provider = nestingProvider("malformed", async (context) => {
    try {
      // Neither result nor error present: violates "exactly one of".
      await context.requestElicitation({ message: "Apply?", requestedSchema: { type: "object" } });
      throw new Error("expected the reply to be rejected as malformed");
    } catch (error) {
      assert.ok(error instanceof NestedRequestError);
      return { code: error.code };
    }
  });
  const harness = createHarness({
    onNestedRequest: (params, { send, end }) => {
      send({ protocol: PROTOCOL, method: "provider/nestedReply", params: { nestedId: params.nestedId } });
      send({ protocol: PROTOCOL, id: 2, method: "provider/shutdown", params: {} });
      end();
    },
  });
  harness.send({ protocol: "maelys-provider/3", id: 1, method: "provider/call", params: { name: "op", arguments: {} } });
  await serveProvider(provider, { input: harness.input, writeLine: harness.writeLine, redirectConsole: false, protectStdout: false });
  assert.deepEqual(harness.output.find((message) => message.id === 1).result.structuredContent, { code: "protocol" });
});

test("the input channel closing mid-wait rejects the nested-request promise as channel_closed", async () => {
  const provider = nestingProvider("closed", async (context) => {
    try {
      await context.requestElicitation({ message: "Apply?", requestedSchema: { type: "object" } });
      throw new Error("expected the channel-closed rejection");
    } catch (error) {
      assert.ok(error instanceof NestedRequestError);
      return { code: error.code };
    }
  });
  const harness = createHarness({
    onNestedRequest: (_params, { end }) => {
      // No reply ever arrives; stdin simply ends, as it would if the host
      // process exited mid-wait.
      end();
    },
  });
  harness.send({ protocol: "maelys-provider/3", id: 1, method: "provider/call", params: { name: "op", arguments: {} } });
  await serveProvider(provider, { input: harness.input, writeLine: harness.writeLine, redirectConsole: false, protectStdout: false });
  assert.deepEqual(harness.output.find((message) => message.id === 1).result.structuredContent, { code: "channel_closed" });
});

test("a second nested request while one is outstanding is refused locally, not sent", async () => {
  const provider = nestingProvider("double", async (context) => {
    const first = context.requestElicitation({ message: "first", requestedSchema: { type: "object" } });
    let secondCode;
    try {
      await context.requestSampling({ messages: [] });
    } catch (error) {
      assert.ok(error instanceof NestedRequestError);
      secondCode = error.code;
    }
    await first.catch(() => undefined);
    return { secondCode };
  });
  const harness = createHarness({
    onNestedRequest: (params, { send, end }) => {
      send({ protocol: PROTOCOL, method: "provider/nestedReply",
        params: { nestedId: params.nestedId, result: { action: "accept" } } });
      send({ protocol: PROTOCOL, id: 2, method: "provider/shutdown", params: {} });
      end();
    },
  });
  harness.send({ protocol: "maelys-provider/3", id: 1, method: "provider/call", params: { name: "op", arguments: {} } });
  await serveProvider(provider, { input: harness.input, writeLine: harness.writeLine, redirectConsole: false, protectStdout: false });
  assert.deepEqual(harness.output.find((message) => message.id === 1).result.structuredContent, { secondCode: "unavailable" });
  // Exactly one nested request ever reached the wire.
  assert.equal(harness.output.filter((message) => message.method === "provider/nestedRequest").length, 1);
});
