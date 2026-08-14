import type { Readable } from "node:stream";

export const PROTOCOL: "maelys-provider/1";
export const TOOL_EFFECTS: readonly ["read", "preview", "apply", "commit", "execute"];

export type JsonValue = null | boolean | number | string | JsonValue[] | { [key: string]: JsonValue };
export type JsonObject = { [key: string]: JsonValue };
export type ToolEffect = typeof TOOL_EFFECTS[number];
export type JsonSchema = JsonObject & { type: "object" | "array" | "string" | "number" | "integer" | "boolean" | "null" };

export type ProviderTool = {
  name: string;
  title?: string;
  description: string;
  inputSchema: JsonSchema;
  outputSchema?: JsonSchema;
  effect: ToolEffect;
  handler(arguments_: JsonObject): unknown | Promise<unknown>;
};

export type Provider = { name: string; version: string; tools: ProviderTool[] };
export type ProviderConfiguration = { name: string; version: string; tools: ProviderTool[] };

export function validateSchemaDefinition(schema: unknown, label?: string): JsonSchema;
export function createProvider(configuration: ProviderConfiguration): Provider;
export function describeProvider(provider: Provider): JsonObject;
export function handleProviderMessage(provider: Provider, message: unknown): Promise<JsonObject>;
export function serveProvider(provider: Provider, options?: {
  input?: Readable;
  writeLine?: (line: string) => void | Promise<void>;
  redirectConsole?: boolean;
}): Promise<number>;
