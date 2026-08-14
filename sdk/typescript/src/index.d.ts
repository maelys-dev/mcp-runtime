import type { Readable } from "node:stream";

export const PROTOCOL: "maelys-provider/2";
export const TOOL_EFFECTS: readonly ["read", "preview", "apply", "commit", "execute"];
export class ProviderNotFoundError extends Error {}

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
export type CallContext = { inputResponses?: JsonObject; requestState?: string; clientCapabilities?: JsonObject };
export type ResourceDescriptor = { uri: string; name: string; title?: string; description?: string; mimeType?: string; size?: number };
export type ResourceTemplateDescriptor = { uriTemplate: string; name: string; title?: string; description?: string; mimeType?: string };
export type ResourceContents = { uri: string; mimeType?: string; _meta?: JsonObject } & ({ text: string } | { blob: string });
export type CompleteResourceResult = { resultType: "complete"; contents: ResourceContents[] };
export type ResourceResult = CompleteResourceResult | InputRequiredResult;
export type ResourceContext = CallContext;

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
  readResource?: (uri: string, context: ResourceContext) => ResourceResult | Promise<ResourceResult>;
};
export type ProviderConfiguration = Omit<Provider, "tools" | "resources" | "resourceTemplates"> & {
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
export function handleProviderMessage(provider: Provider, message: unknown): Promise<JsonObject>;
export function serveProvider(provider: Provider, options?: {
  input?: Readable;
  writeLine?: (line: string) => void | Promise<void>;
  redirectConsole?: boolean;
}): Promise<number>;
