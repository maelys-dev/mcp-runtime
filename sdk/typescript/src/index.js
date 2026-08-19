import readline from "node:readline";

export const PROTOCOL = "maelys-provider/4";
/* The version every provider speaks, and the one the host opens with until
   this SDK declares the newer one in a response. */
export const PROTOCOL_FLOOR = "maelys-provider/3";
export const TOOL_EFFECTS = Object.freeze(["read", "preview", "apply", "commit", "execute"]);
const EVENT_CONTROL = Symbol("maelys-provider-event-control");

export class ProviderNotFoundError extends Error {
  constructor(message = "resource not found") {
    super(message);
    this.name = "ProviderNotFoundError";
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

export async function handleProviderMessage(provider, message) {
  const request = objectValue(message, "provider request");
  if (typeof request.id !== "number" || !Number.isInteger(request.id)) throw new TypeError("provider request id must be an integer");
  /* The host opens at the floor and only speaks the current version once we
   have declared it in a response, so both are valid inbound. */
  if (request.protocol !== PROTOCOL && request.protocol !== PROTOCOL_FLOOR) {
    throw new TypeError("unsupported provider protocol");
  }
  const params = objectValue(request.params ?? {}, "provider request params");
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
  const lines = readline.createInterface({ input, crlfDelay: Infinity });
  try {
    for await (const line of lines) {
      let message;
      let response;
      try {
        message = JSON.parse(line);
        response = await handleProviderMessage(provider, message);
      } catch (error) {
        const candidate = message && typeof message === "object" && !Array.isArray(message) ? message.id : 0;
        const id = typeof candidate === "number" && Number.isInteger(candidate) ? candidate : 0;
        response = { protocol: PROTOCOL, id, error: {
          code: error instanceof ProviderNotFoundError ? "not_found" : "provider_error",
          message: error instanceof Error ? error.message : String(error),
        } };
      }
      await writeMessage(response);
      if (message && typeof message === "object" && !Array.isArray(message) &&
          message.method === "provider/activate" && response.result) {
        eventControl.activate();
      }
      if (message && typeof message === "object" && !Array.isArray(message) && message.method === "provider/shutdown") break;
    }
  } finally {
    lines.close();
    eventControl.deactivate();
    await writeTail;
    if (options.redirectConsole !== false) Object.assign(console, originalConsole);
  }
  return 0;
}
