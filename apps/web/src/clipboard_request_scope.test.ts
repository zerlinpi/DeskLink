import { describe, expect, it } from "vitest";
import { resetClipboardRequestsForChannelChange } from "./clipboard_request_scope";

describe("resetClipboardRequestsForChannelChange", () => {
  it("cancels requests that belong to a replaced control channel", () => {
    const pending = new Map([
      ["read-1", "remote-to-local" as const],
      ["write-1", "local-to-remote" as const],
    ]);
    const oldChannel = {};
    const newChannel = {};

    expect(resetClipboardRequestsForChannelChange(pending, oldChannel, newChannel)).toBe(true);
    expect(pending.size).toBe(0);
  });

  it("keeps requests when the same channel is announced again", () => {
    const pending = new Map([["read-1", "remote-to-local" as const]]);
    const channel = {};

    expect(resetClipboardRequestsForChannelChange(pending, channel, channel)).toBe(false);
    expect(pending.size).toBe(1);
  });

  it("does not report cancellation on the initial channel attach", () => {
    const pending = new Map<string, "local-to-remote" | "remote-to-local">();

    expect(resetClipboardRequestsForChannelChange(pending, null, {})).toBe(false);
    expect(pending.size).toBe(0);
  });
});
