import readline from "node:readline";

/*
 * Every version between the floor and the current one, oldest first, so the
 * list - not just its endpoints - is what "supported" means: a host that
 * only accepted the floor and the newest would reject a provider still
 * declaring an in-between version the moment a newer one existed. This
 * mirrors the same array in src/provider/process_provider.c.
 */
export const PROTOCOL = "maelys-provider/5";
/* The version every provider speaks, and the one the host opens with until
   this SDK declares the newer one in a response. */
export const PROTOCOL_FLOOR = "maelys-provider/3";
const SUPPORTED_PROTOCOLS = Object.freeze([PROTOCOL_FLOOR, "maelys-provider/4", PROTOCOL]);
export const TOOL_EFFECTS = Object.freeze(["read", "preview", "apply", "commit", "execute"]);
/* The three methods the host will ever relay for a nested request - the same
   set, and the same client-capability gate, the input_required path uses. */
export const NESTED_REQUEST_METHODS = Object.freeze(["elicitation/create", "sampling/createMessage", "roots/list"]);
const EVENT_CONTROL = Symbol("maelys-provider-event-control");

export class ProviderNotFoundError extends Error {
  constructor(message = "resource not found") {
    super(message);
    this.name = "ProviderNotFoundError";
  }
}

/*
 * Raised by requestElicitation/requestSampling/requestRoots. `code` is either
 * one the host's provider/nestedReply reported verbatim - "client_error"
 * (the client's own JSON-RPC error, carried on as `.data`), "denied",
 * "timeout", "cancelled" or "unavailable" (docs/provider-protocol.md's
 * exact vocabulary) - or one this SDK synthesizes locally because no reply
 * will ever arrive to report one: "channel_closed" (provider stdin ended
 * mid-wait) and "protocol" (the reply that did arrive did not fit the wire
 * shape). Keeping every rejection in one error type, with `code` always
 * present, means a handler can catch once regardless of which side noticed
 * the failure.
 */
export class NestedRequestError extends Error {
  constructor(code, message, data) {
    super(message || "nested request failed");
    this.name = "NestedRequestError";
    this.code = code;
    if (data !== undefined) this.data = data;
  }
}

const schemaKeys = new Set([
  "$schema", "title", "description", "type", "properties", "required",
  "additionalProperties", "items", "enum", "minLength", "maxLength", "minimum", "maximum",
]);
const schemaTypes = new Set(["object", "array", "string", "number", "integer", "boolean", "null"]);

function matchesType(type, value) {
  if (type === "object") return value !== null && typeof value === "object" && !Array.isArray(value);
  if (type === "array") return Array.isArray(value);
  if (type === "string") return typeof value === "string";
  if (type === "number") return typeof value === "number" && Number.isFinite(value);
  if (type === "integer") return typeof value === "number" && Number.isInteger(value);
  if (type === "boolean") return typeof value === "boolean";
  return value === null;
}

function objectValue(value, label) {
  if (!value || typeof value !== "object" || Array.isArray(value)) throw new TypeError(`${label} must be an object`);
  return value;
}

function stringValue(value, label) {
  if (typeof value !== "string" || value.length === 0) throw new TypeError(`${label} must be a non-empty string`);
  return value;
}

function createEventControl() {
  let writeMessage;
  let active = false;
  const emit = async (method, params = {}) => {
    if (!active || typeof writeMessage !== "function") {
      throw new Error("provider events are unavailable before activation or after shutdown");
    }
    await writeMessage({ protocol: PROTOCOL, method, params });
  };
  return {
    public: Object.freeze({
      resourceUpdated(uri) {
        return emit("provider/notifications/resources/updated", {
          uri: stringValue(uri, "resource event uri"),
        });
      },
      resourcesListChanged() {
        return emit("provider/notifications/resources/list_changed");
      },
      toolsListChanged() {
        return emit("provider/notifications/tools/list_changed");
      },
    }),
    bind(writer) { writeMessage = writer; },
    activate() { active = true; },
    deactivate() { active = false; writeMessage = undefined; },
  };
}

export function validateSchemaDefinition(schema, label = "schema") {
  const value = objectValue(schema, label);
  for (const key of Object.keys(value)) if (!schemaKeys.has(key)) throw new TypeError(`${label} uses unsupported key: ${key}`);
  if (!schemaTypes.has(value.type)) throw new TypeError(`${label}.type is missing or unsupported`);
  if ((value.properties !== undefined || value.required !== undefined || value.additionalProperties !== undefined) && value.type !== "object") {
    throw new TypeError(`${label}: object keywords require type object`);
  }
  if (value.properties !== undefined) {
    if (value.type !== "object") throw new TypeError(`${label}.properties requires an object schema`);
    const properties = objectValue(value.properties, `${label}.properties`);
    for (const [name, child] of Object.entries(properties)) validateSchemaDefinition(child, `${label}.properties.${name}`);
  }
  if (value.required !== undefined) {
    if (value.type !== "object" || !Array.isArray(value.required) || !value.required.every((item) => typeof item === "string")) {
      throw new TypeError(`${label}.required must be a string array on an object schema`);
    }
    if (new Set(value.required).size !== value.required.length) throw new TypeError(`${label}.required contains duplicates`);
    const properties = value.properties ?? {};
    if (value.required.some((name) => !(name in properties))) throw new TypeError(`${label}.required references an undeclared property`);
  }
  if (value.additionalProperties !== undefined && typeof value.additionalProperties !== "boolean") {
    throw new TypeError(`${label}.additionalProperties must be boolean`);
  }
  if (value.items !== undefined) {
    if (value.type !== "array") throw new TypeError(`${label}.items requires an array schema`);
    validateSchemaDefinition(value.items, `${label}.items`);
  }
  if (value.enum !== undefined && (!Array.isArray(value.enum) || value.enum.length === 0)) {
    throw new TypeError(`${label}.enum must be a non-empty array`);
  }
  if (Array.isArray(value.enum) && value.enum.some((candidate) => !matchesType(value.type, candidate))) {
    throw new TypeError(`${label}.enum contains a value that does not match type ${value.type}`);
  }
  if ((value.minLength !== undefined || value.maxLength !== undefined) && value.type !== "string") {
    throw new TypeError(`${label}: length keywords require type string`);
  }
  for (const keyword of ["minLength", "maxLength"]) {
    if (value[keyword] !== undefined && (!Number.isInteger(value[keyword]) || value[keyword] < 0)) {
      throw new TypeError(`${label}.${keyword} must be a non-negative integer`);
    }
  }
  if (value.minLength !== undefined && value.maxLength !== undefined && value.minLength > value.maxLength) {
    throw new TypeError(`${label} has inconsistent string bounds`);
  }
  for (const keyword of ["minimum", "maximum"]) {
    if (value[keyword] !== undefined && typeof value[keyword] !== "number") throw new TypeError(`${label}.${keyword} must be a number`);
  }
  if ((value.minimum !== undefined || value.maximum !== undefined) && !["number", "integer"].includes(value.type)) {
    throw new TypeError(`${label}: numeric bounds require type number or integer`);
  }
  if (value.minimum !== undefined && value.maximum !== undefined && value.minimum > value.maximum) {
    throw new TypeError(`${label} has inconsistent numeric bounds`);
  }
  return value;
}

export function createProvider(configuration) {
  const value = objectValue(configuration, "provider");
  const name = stringValue(value.name, "provider.name");
  const version = stringValue(value.version, "provider.version");
  const configuredTools = value.tools ?? [];
  if (!Array.isArray(configuredTools)) throw new TypeError("provider.tools must be an array");
  const names = new Set();
  const tools = configuredTools.map((tool, index) => {
    const candidate = objectValue(tool, `tools[${index}]`);
    const toolName = stringValue(candidate.name, `tools[${index}].name`);
    if (names.has(toolName)) throw new TypeError(`duplicate provider tool: ${toolName}`);
    names.add(toolName);
    stringValue(candidate.description, `tools[${index}].description`);
    if (!TOOL_EFFECTS.includes(candidate.effect)) throw new TypeError(`tools[${index}].effect is unsupported`);
    validateSchemaDefinition(candidate.inputSchema, `tools[${index}].inputSchema`);
    if (candidate.inputSchema.type !== "object") throw new TypeError(`tools[${index}].inputSchema.type must be object`);
    if (candidate.outputSchema !== undefined) validateSchemaDefinition(candidate.outputSchema, `tools[${index}].outputSchema`);
    if (typeof candidate.handler !== "function") throw new TypeError(`tools[${index}].handler must be a function`);
    return { ...candidate };
  });
  const resources = value.resources ?? [];
  const resourceTemplates = value.resourceTemplates ?? [];
  if (!Array.isArray(resources) || !Array.isArray(resourceTemplates)) {
    throw new TypeError("provider resources and resourceTemplates must be arrays");
  }
  const resourceUris = new Set();
  const preparedResources = resources.map((entry, index) => {
    const resource = objectValue(entry, `resources[${index}]`);
    const uri = stringValue(resource.uri, `resources[${index}].uri`);
    stringValue(resource.name, `resources[${index}].name`);
    if (resourceUris.has(uri)) throw new TypeError(`duplicate provider resource: ${uri}`);
    resourceUris.add(uri);
    if (resource.size !== undefined && (!Number.isSafeInteger(resource.size) || resource.size < 0)) {
      throw new TypeError(`resources[${index}].size must be a non-negative integer`);
    }
    return { ...resource };
  });
  const templateUris = new Set();
  const preparedTemplates = resourceTemplates.map((entry, index) => {
    const resource = objectValue(entry, `resourceTemplates[${index}]`);
    const uriTemplate = stringValue(resource.uriTemplate, `resourceTemplates[${index}].uriTemplate`);
    stringValue(resource.name, `resourceTemplates[${index}].name`);
    if (templateUris.has(uriTemplate)) throw new TypeError(`duplicate provider resource template: ${uriTemplate}`);
    templateUris.add(uriTemplate);
    return { ...resource };
  });
  if ((preparedResources.length || preparedTemplates.length) && typeof value.readResource !== "function") {
    throw new TypeError("provider.readResource is required when resources are declared");
  }
  const eventControl = createEventControl();
  return { name, version, tools, resources: preparedResources,
    resourceTemplates: preparedTemplates, readResource: value.readResource,
    events: eventControl.public, [EVENT_CONTROL]: eventControl };
}

export function describeProvider(provider) {
  const description = {
    name: provider.name,
    version: provider.version,
    tools: provider.tools.map(({ handler: _handler, ...descriptor }) => descriptor),
  };
  if (provider.resources.length) description.resources = provider.resources;
  if (provider.resourceTemplates.length) description.resourceTemplates = provider.resourceTemplates;
  return description;
}

export function completeResult({ content, structuredContent, isError = false }) {
  if (content === undefined && structuredContent === undefined) {
    throw new TypeError("complete result requires content or structuredContent");
  }
  if (content !== undefined && (!Array.isArray(content) || content.length === 0)) {
    throw new TypeError("complete result content must be a non-empty array");
  }
  return {
    resultType: "complete",
    ...(content === undefined ? {} : { content }),
    ...(structuredContent === undefined ? {} : { structuredContent }),
    ...(isError ? { isError: true } : {}),
  };
}

export function inputRequiredResult({ inputRequests, requestState }) {
  if (inputRequests === undefined && requestState === undefined) {
    throw new TypeError("input_required result requires inputRequests or requestState");
  }
  if (inputRequests !== undefined) {
    objectValue(inputRequests, "inputRequests");
    if (Object.keys(inputRequests).length === 0) throw new TypeError("inputRequests must not be empty");
  }
  if (requestState !== undefined) stringValue(requestState, "requestState");
  return {
    resultType: "input_required",
    ...(inputRequests === undefined ? {} : { inputRequests }),
    ...(requestState === undefined ? {} : { requestState }),
  };
}

export function resourceResult(contents) {
  if (!Array.isArray(contents) || contents.length === 0) {
    throw new TypeError("resource result contents must be a non-empty array");
  }
  return { resultType: "complete", contents };
}

function validateProviderResult(result) {
  const value = objectValue(result, "provider result");
  if (value.resultType === "complete") {
    return completeResult(value);
  }
  if (value.resultType === "input_required") {
    return inputRequiredResult(value);
  }
  throw new TypeError("provider resultType must be complete or input_required");
}

/*
 * The helper a handler calls to open one request back at the client and
 * block for the answer (docs/provider-protocol.md "Nested requests
 * (version 5)"). `connection` is the single per-process-connection slot
 * serveProvider owns; there is exactly one because the wire is strictly
 * single-outstanding in both directions and this SDK never runs two calls
 * concurrently (the dispatch loop awaits one handler to completion before
 * reading the next request), so a second nested request can only mean a
 * handler that raced its own two helper calls - refused locally, before a
 * byte reaches the wire, the same way a double nested request coming back
 * the other way would be fatal to the whole transport.
 */
function sendNestedRequest(connection, method, params) {
  if (connection.nestedWaiter) {
    return Promise.reject(new NestedRequestError("unavailable",
      "a nested request is already outstanding on this connection"));
  }
  const nestedId = `n${++connection.nestedCounter}`;
  const reply = new Promise((resolve, reject) => {
    connection.nestedWaiter = { nestedId, resolve, reject };
  });
  const innerParams = params === undefined ? {} : objectValue(params, `${method} params`);
  Promise.resolve(connection.writeMessage({
    protocol: PROTOCOL,
    method: "provider/nestedRequest",
    params: { nestedId, method, params: innerParams },
  })).catch((error) => {
    // The write itself failed (e.g. stdout closed): nothing will ever answer
    // this nestedId, so settle the waiter here rather than leaving it to the
    // channel-closed path, which only fires once the *input* side notices.
    if (connection.nestedWaiter && connection.nestedWaiter.nestedId === nestedId) {
      connection.nestedWaiter = null;
      reply.catch(() => undefined);
    }
  });
  return reply;
}

function unavailableNestedRequest(name) {
  return () => Promise.reject(new NestedRequestError("unavailable",
    `${name} requires a live connection - it is unavailable outside serveProvider`));
}

/*
 * Resolves (or rejects) whichever requestElicitation/requestSampling/
 * requestRoots call is currently waiting, from a provider/nestedReply frame
 * the frame pump in serveProvider just read off stdin. `nestedId` is the
 * collision-avoidance mechanism the wire uses instead of an id-space
 * negotiation (docs/provider-protocol.md), so a mismatch here means the
 * host and this SDK have lost track of which reply answers which request -
 * treated the same as any other malformed reply, not silently dropped.
 */
function resolveNestedReply(connection, message) {
  const waiter = connection.nestedWaiter;
  if (!waiter) {
    diagnostic(["ignoring unexpected provider/nestedReply", message]);
    return;
  }
  const params = message && typeof message.params === "object" && message.params !== null &&
    !Array.isArray(message.params) ? message.params : null;
  const nestedId = params ? params.nestedId : undefined;
  if (typeof nestedId !== "string" || nestedId !== waiter.nestedId) {
    connection.nestedWaiter = null;
    waiter.reject(new NestedRequestError("protocol",
      `provider/nestedReply nestedId did not match the outstanding request (expected ${waiter.nestedId})`));
    return;
  }
  connection.nestedWaiter = null;
  const hasResult = params && Object.prototype.hasOwnProperty.call(params, "result");
  const hasError = params && Object.prototype.hasOwnProperty.call(params, "error");
  if (hasResult === hasError) {
    waiter.reject(new NestedRequestError("protocol",
      "provider/nestedReply must contain exactly one of result or error"));
    return;
  }
  if (hasError) {
    const error = params.error;
    if (!error || typeof error !== "object" || Array.isArray(error) ||
        typeof error.code !== "string" || typeof error.message !== "string") {
      waiter.reject(new NestedRequestError("protocol",
        "provider/nestedReply error must have a string code and message"));
      return;
    }
    waiter.reject(new NestedRequestError(error.code, error.message, error.data));
    return;
  }
  waiter.resolve(params.result);
}

/* Settles whatever nested wait is outstanding when the connection can no
   longer produce a reply - stdin closed or errored mid-wait. */
function failNestedWaiter(connection, error) {
  const waiter = connection.nestedWaiter;
  if (!waiter) return;
  connection.nestedWaiter = null;
  waiter.reject(error);
}

/*
 * A tiny async FIFO, not a library: push() never blocks, take() resolves in
 * arrival order (queued items first, otherwise the next push), and close()
 * makes every future and outstanding take() resolve to `undefined` - the
 * dispatch loop's "no more frames" signal, mirroring an exhausted async
 * iterator's `{done: true}`.
 */
function createAsyncQueue() {
  const items = [];
  const waiters = [];
  let closed = false;
  return {
    push(item) {
      if (closed) return;
      if (waiters.length) waiters.shift()(item);
      else items.push(item);
    },
    close() {
      if (closed) return;
      closed = true;
      while (waiters.length) waiters.shift()(undefined);
    },
    take() {
      if (items.length) return Promise.resolve(items.shift());
      if (closed) return Promise.resolve(undefined);
      return new Promise((resolve) => waiters.push(resolve));
    },
  };
}

/*
 * `nested` binds the three request-back helpers to a live connection; when
 * omitted (every direct call in this SDK's own unit tests, and any embedder
 * that only wants request/response dispatch) each helper rejects immediately
 * rather than silently hanging, so a handler discovers the gap the first
 * time it calls one rather than only under a real nested-capable host.
 */
export async function handleProviderMessage(provider, message, nested = {}) {
  const request = objectValue(message, "provider request");
  if (typeof request.id !== "number" || !Number.isInteger(request.id)) throw new TypeError("provider request id must be an integer");
  /* The host opens at the floor and only speaks a newer version once we have
     declared it in a frame, so any version in the supported range is valid
     inbound - not only the floor and the current one. */
  if (!SUPPORTED_PROTOCOLS.includes(request.protocol)) {
    throw new TypeError("unsupported provider protocol");
  }
  const params = objectValue(request.params ?? {}, "provider request params");
  const requestElicitation = nested.requestElicitation ?? unavailableNestedRequest("requestElicitation");
  const requestSampling = nested.requestSampling ?? unavailableNestedRequest("requestSampling");
  const requestRoots = nested.requestRoots ?? unavailableNestedRequest("requestRoots");
  let result;
  if (request.method === "provider/describe") {
    result = describeProvider(provider);
  } else if (request.method === "provider/activate") {
    result = {};
  } else if (request.method === "provider/call") {
    const name = stringValue(params.name, "provider/call name");
    const arguments_ = objectValue(params.arguments ?? {}, "provider/call arguments");
    const tool = provider.tools.find((candidate) => candidate.name === name);
    if (!tool) throw new Error(`unknown provider tool: ${name}`);
    const context = {
      inputResponses: params.inputResponses === undefined ? undefined :
        objectValue(params.inputResponses, "provider/call inputResponses"),
      requestState: params.requestState === undefined ? undefined :
        stringValue(params.requestState, "provider/call requestState"),
      clientCapabilities: params.clientCapabilities === undefined ? undefined :
        objectValue(params.clientCapabilities, "provider/call clientCapabilities"),
      requestElicitation, requestSampling, requestRoots,
    };
    result = validateProviderResult(await tool.handler(arguments_, context));
  } else if (request.method === "provider/readResource") {
    if (typeof provider.readResource !== "function") throw new Error("provider does not expose resources");
    const uri = stringValue(params.uri, "provider/readResource uri");
    const context = {
      inputResponses: params.inputResponses === undefined ? undefined :
        objectValue(params.inputResponses, "provider/readResource inputResponses"),
      requestState: params.requestState === undefined ? undefined :
        stringValue(params.requestState, "provider/readResource requestState"),
      clientCapabilities: params.clientCapabilities === undefined ? undefined :
        objectValue(params.clientCapabilities, "provider/readResource clientCapabilities"),
      requestElicitation, requestSampling, requestRoots,
    };
    const candidate = await provider.readResource(uri, context);
    result = candidate?.resultType === "input_required" ?
      inputRequiredResult(candidate) : resourceResult(candidate?.contents);
  } else if (request.method === "provider/shutdown") {
    result = {};
  } else {
    throw new Error(`unknown provider method: ${String(request.method)}`);
  }
  return { protocol: PROTOCOL, id: request.id, result };
}

function diagnostic(values) {
  process.stderr.write(`${values.map((value) => typeof value === "string" ? value : JSON.stringify(value)).join(" ")}\n`);
}

export async function serveProvider(provider, options = {}) {
  const input = options.input ?? process.stdin;
  const writeLine = options.writeLine ?? ((line) => process.stdout.write(`${line}\n`));
  const eventControl = provider?.[EVENT_CONTROL];
  if (!eventControl) throw new TypeError("provider was not created by createProvider");
  let writeTail = Promise.resolve();
  const writeMessage = (message) => {
    const encoded = JSON.stringify(message);
    const pending = writeTail.then(() => writeLine(encoded));
    writeTail = pending.catch(() => undefined);
    return pending;
  };
  eventControl.bind(writeMessage);
  const originalConsole = { log: console.log, info: console.info, warn: console.warn };
  if (options.redirectConsole !== false) {
    console.log = (...values) => diagnostic(values);
    console.info = (...values) => diagnostic(values);
    console.warn = (...values) => diagnostic(values);
  }

  /*
   * One raw line reader for the whole connection, pulled out of the dispatch
   * loop so both it and an in-call requestElicitation/requestSampling/
   * requestRoots can consume frames without either losing or reordering one.
   * It never stops pumping stdin: every line is classified the moment it
   * arrives - a provider/nestedReply is resolved right here, in arrival
   * order, whether the dispatch loop below is idle between requests or a
   * handler is blocked deeper on the stack waiting on exactly this frame;
   * everything else is handed to the dispatch queue untouched. That keeps
   * ordinary requests from ever being reordered around a nested detour, and
   * is the same three-way split process_reader_main makes on the host's own
   * provider-facing leg (response / nested request / event) - here there are
   * only two kinds, because host-to-provider traffic has no third, event-like
   * frame to distinguish.
   */
  const connection = { writeMessage, nestedWaiter: null, nestedCounter: 0 };
  const dispatchQueue = createAsyncQueue();
  const lines = readline.createInterface({ input, crlfDelay: Infinity });
  const pump = (async () => {
    try {
      for await (const line of lines) {
        let parsed;
        let parseError;
        try {
          parsed = JSON.parse(line);
        } catch (error) {
          parseError = error;
        }
        if (parseError === undefined && parsed && typeof parsed === "object" && !Array.isArray(parsed) &&
            parsed.id === undefined && parsed.method === "provider/nestedReply") {
          resolveNestedReply(connection, parsed);
          continue;
        }
        dispatchQueue.push({ parsed, parseError });
      }
    } finally {
      dispatchQueue.close();
      failNestedWaiter(connection, new NestedRequestError("channel_closed",
        "provider input closed before a nested reply arrived"));
    }
  })();

  try {
    for (;;) {
      const next = await dispatchQueue.take();
      if (next === undefined) break;
      const { parsed: message, parseError } = next;
      let response;
      try {
        if (parseError !== undefined) throw parseError;
        response = await handleProviderMessage(provider, message, {
          requestElicitation: (params) => sendNestedRequest(connection, "elicitation/create", params),
          requestSampling: (params) => sendNestedRequest(connection, "sampling/createMessage", params),
          requestRoots: (params) => sendNestedRequest(connection, "roots/list", params),
        });
      } catch (error) {
        const candidate = message && typeof message === "object" && !Array.isArray(message) ? message.id : 0;
        const id = typeof candidate === "number" && Number.isInteger(candidate) ? candidate : 0;
        response = { protocol: PROTOCOL, id, error: {
          code: error instanceof ProviderNotFoundError ? "not_found" :
            error instanceof NestedRequestError ? error.code : "provider_error",
          message: error instanceof Error ? error.message : String(error),
        } };
      }
      await writeMessage(response);
      if (message && typeof message === "object" && !Array.isArray(message) &&
          message.method === "provider/activate" && response.result) {
        eventControl.activate();
      }
      if (message && typeof message === "object" && !Array.isArray(message) && message.method === "provider/shutdown") {
        lines.close();
        break;
      }
    }
  } finally {
    lines.close();
    eventControl.deactivate();
    await writeTail;
    // Cleanup below must run whether or not the pump ever saw a transport
    // error, so its rejection (a stream `error` event, propagated the same
    // way an unguarded `for await` would surface one) is captured rather
    // than awaited directly, and only rethrown once every other finally
    // step - restoring console among them - has already executed.
    const pumpFailure = await pump.then(() => undefined, (error) => error);
    if (options.redirectConsole !== false) Object.assign(console, originalConsole);
    if (pumpFailure) throw pumpFailure;
  }
  return 0;
}
