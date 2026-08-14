import assert from "node:assert/strict";
import test from "node:test";
import { completeResult, createProvider, describeProvider, handleProviderMessage, inputRequiredResult, resourceResult, validateSchemaDefinition } from "../src/index.js";

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
    protocol: "maelys-provider/2",
    id: 7,
    method: "provider/call",
    params: { name: "fixture.echo", arguments: { message: "hello" } },
  });
  assert.deepEqual(response, { protocol: "maelys-provider/2", id: 7, result: {
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
    protocol: "maelys-provider/2",
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
    protocol: "maelys-provider/2", id: 1, method: "provider/call",
    params: { name: "confirm", arguments: {} },
  });
  assert.equal(first.result.resultType, "input_required");
  const second = await handleProviderMessage(provider, {
    protocol: "maelys-provider/2", id: 2, method: "provider/call",
    params: { name: "confirm", arguments: {}, inputResponses: { confirm: { action: "accept" } }, requestState: "opaque" },
  });
  assert.equal(second.result.resultType, "complete");
  assert.deepEqual(seen, {
    inputResponses: { confirm: { action: "accept" } },
    requestState: "opaque",
    clientCapabilities: undefined,
  });
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
    protocol: "maelys-provider/2", id: 10, method: "provider/readResource",
    params: { uri: "fixture://echo/hello" },
  });
  assert.deepEqual(response.result, { resultType: "complete", contents: [{
    uri: "fixture://echo/hello", mimeType: "text/plain", text: "read fixture://echo/hello",
  }] });
});
