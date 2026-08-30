import {
  parseHostCapabilitiesMessage,
  type HostCapabilitiesV1,
} from "./host_capabilities";

export type ControlChannelMessage =
  | { kind: "host-capabilities"; capabilities: HostCapabilitiesV1 }
  | { kind: "other" }
  | { kind: "invalid" };

const MAX_CONTROL_MESSAGE_BYTES = 64 * 1024;

export function decodeControlChannelText(text: string): ControlChannelMessage {
  if (!text || text.length > MAX_CONTROL_MESSAGE_BYTES) return { kind: "invalid" };

  let value: unknown;
  try {
    value = JSON.parse(text) as unknown;
  } catch {
    return { kind: "invalid" };
  }

  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    return { kind: "invalid" };
  }

  const record = value as Record<string, unknown>;
  if (record.t !== "host-capabilities") return { kind: "other" };

  const capabilities = parseHostCapabilitiesMessage(record);
  return capabilities
    ? { kind: "host-capabilities", capabilities }
    : { kind: "invalid" };
}
