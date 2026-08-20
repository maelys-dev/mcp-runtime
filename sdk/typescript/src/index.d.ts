import type { Readable } from "node:stream";

/** The newest version this SDK ever declares - once it has actually opened a
 *  nested request. Also the ceiling of what it accepts inbound. */
export const PROTOCOL: "maelys-provider/5";
export const PROTOCOL_FLOOR: "maelys-provider/3";
/** Declared on every outbound frame until a handler opens its first nested
 *  request; raised to PROTOCOL then, never lowered. A provider that never
 *  nests keeps declaring this version for the whole session. */
export const PROTOCOL_DECLARED: "maelys-provider/4";
export const TOOL_EFFECTS: readonly ["read", "preview", "apply", "commit", "execute"];
export const NESTED_REQUEST_METHODS: readonly ["elicitation/create", "sampling/createMessage", "roots/list"];
export class ProviderNotFoundError extends Error {}

/**
 * Rejection type for requestElicitation/requestSampling/requestRoots.
 * `client_error` | `denied` | `timeout` | `cancelled` | `unavailable` |
 * `failed` come verbatim from the host's own `provider/nestedReply` error
 * vocabulary (docs/provider-protocol.md); `channel_closed` and `protocol`
 * are synthesized locally when no such reply will ever arrive, or the one
 * that arrived did not fit the wire shape.
 */
export type NestedRequestErrorCode =
  | "client_error" | "denied" | "timeout" | "cancelled" | "unavailable" | "failed"
  | "channel_closed" | "protocol";
export class NestedRequestError extends Error {
  code: NestedRequestErrorCode;
  data?: JsonValue;
}

export type JsonValue = null | boolean | number | string | JsonValue[] | { [key: string]: JsonValue };
export type JsonObject = { [key: string]: JsonValue };
export type ToolEffect = typeof TOOL_EFFECTS[number];
export type JsonSchema = JsonObject & { type: "object" | "array" | "string" | "number" | "integer" | "boolean" | "null" };
export type TextContent = { type: "text"; text: string; annotations?: JsonObject; _meta?: JsonObject };
export type ImageContent = { type: "image"; data: string; mimeType: `image/${string}`; annotations?: JsonObject; _meta?: JsonObject };
export type AudioContent = { type: "audio"; data: string; mimeType: `audio/${string}`; annotations?: JsonObject; _meta?: JsonObject };
export type ResourceLinkContent = { type: "resource_link"; name: string; uri: string; title?: string; description?: string; mimeType?: string; annotations?: JsonObject; _meta?: JsonObject };
export type EmbeddedResourceContent = { type: "resource"; resource: ({ uri: string; mimeType?: string; text: string } | { uri: string; mimeType?: string; blob: string }); annotations?: JsonObject; _meta?: JsonObject };
export type ContentBlock = TextContent | ImageContent | AudioContent | ResourceLinkContent | EmbeddedResourceContent;
export type CompleteResult = { resultType: "complete"; content?: ContentBlock[]; structuredContent?: JsonValue; isError?: boolean };
export type InputRequest = { method: "elicitation/create" | "sampling/createMessage" | "roots/list"; params: JsonObject };
export type InputRequiredResult = { resultType: "input_required"; inputRequests?: Record<string, InputRequest>; requestState?: string };
export type ProviderResult = CompleteResult | InputRequiredResult;

/* The three methods the host will relay a nested request for
   (docs/provider-protocol.md "Nested requests (version 5)"); params/result
   shapes are MCP's own, not this SDK's, so they stay loosely typed JSON
   except where the wire's own envelope is fixed (elicitation's action,
   roots' array). */
export type ElicitationRequestParams = JsonObject & { message: string; requestedSchema: JsonSchema };
export type ElicitationResult = { action: "accept" | "decline" | "cancel"; content?: JsonObject };
export type SamplingRequestParams = JsonObject;
export type SamplingResult = JsonObject;
export type RootsRequestParams = JsonObject;
export type RootsResult = { roots: Array<{ uri: string; name?: string }> };

export type CallContext = {
  inputResponses?: JsonObject;
  requestState?: string;
  clientCapabilities?: JsonObject;
  /** Opens one request back at the client and blocks for the answer; rejects
   *  with NestedRequestError on denial, timeout or a closed channel. */
  requestElicitation(params: ElicitationRequestParams): Promise<ElicitationResult>;
  requestSampling(params: SamplingRequestParams): Promise<SamplingResult>;
  requestRoots(params?: RootsRequestParams): Promise<RootsResult>;
};
export type ResourceDescriptor = { uri: string; name: string; title?: string; description?: string; mimeType?: string; size?: number };
export type ResourceTemplateDescriptor = { uriTemplate: string; name: string; title?: string; description?: string; mimeType?: string };
export type ResourceContents = { uri: string; mimeType?: string; _meta?: JsonObject } & ({ text: string } | { blob: string });
export type CompleteResourceResult = { resultType: "complete"; contents: ResourceContents[] };
export type ResourceResult = CompleteResourceResult | InputRequiredResult;
export type ResourceContext = CallContext;
export type ProviderEvents = {
  resourceUpdated(uri: string): Promise<void>;
  resourcesListChanged(): Promise<void>;
  toolsListChanged(): Promise<void>;
};

export type ProviderTool = {
  name: string;
  title?: string;
  description: string;
  inputSchema: JsonSchema;
  outputSchema?: JsonSchema;
  effect: ToolEffect;
  handler(arguments_: JsonObject, context: CallContext): ProviderResult | Promise<ProviderResult>;
};

export type Provider = {
  name: string;
  version: string;
  tools: ProviderTool[];
  resources: ResourceDescriptor[];
  resourceTemplates: ResourceTemplateDescriptor[];
  events: ProviderEvents;
  readResource?: (uri: string, context: ResourceContext) => ResourceResult | Promise<ResourceResult>;
};
export type ProviderConfiguration = Omit<Provider, "tools" | "resources" | "resourceTemplates" | "events"> & {
  tools?: ProviderTool[];
  resources?: ResourceDescriptor[];
  resourceTemplates?: ResourceTemplateDescriptor[];
};

export function validateSchemaDefinition(schema: unknown, label?: string): JsonSchema;
export function createProvider(configuration: ProviderConfiguration): Provider;
export function describeProvider(provider: Provider): JsonObject;
export function completeResult(value: Omit<CompleteResult, "resultType">): CompleteResult;
export function inputRequiredResult(value: Omit<InputRequiredResult, "resultType">): InputRequiredResult;
export function resourceResult(contents: ResourceContents[]): CompleteResourceResult;
export type NestedRequestBindings = {
  requestElicitation?: (params: ElicitationRequestParams) => Promise<ElicitationResult>;
  requestSampling?: (params: SamplingRequestParams) => Promise<SamplingResult>;
  requestRoots?: (params?: RootsRequestParams) => Promise<RootsResult>;
};
export function handleProviderMessage(provider: Provider, message: unknown, nested?: NestedRequestBindings): Promise<JsonObject>;
export function serveProvider(provider: Provider, options?: {
  input?: Readable;
  writeLine?: (line: string) => void | Promise<void>;
  redirectConsole?: boolean;
}): Promise<number>;
