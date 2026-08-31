import { describe, expect, it } from "vitest";

import { decodeControlChannelText } from "./control_channel_message";

function nestedCapabilitiesMessage() {
  return {
    t: "host-capabilities",
    version: 1,
    capabilities: {
      version: 1,
      secureAttention: { available: false, reason: "policy-not-allowed" },
      clipboard: { available: true },
      fileTransfer: { available: true },
      systemAudio: { available: false },
      microphone: { available: false },
      protectedDesktop: { available: false },
      multiMonitor: { available: true },
      highRefresh: { available: true },
      virtualDisplay: { available: false },
      privacyMode: { available: false },
      virtualHid: { available: false },
      gamepad: { available: false },
      codecs: [{ codec: "h264", maximumFps: 144 }],
      maximumFps: 144,
      maximumResolution: { width: 3840, height: 2160 },
    },
  };
}

describe("control channel message decoding", () => {
  it("extracts a valid HostCapabilitiesV1 advertisement", () => {
    const result = decodeControlChannelText(JSON.stringify(nestedCapabilitiesMessage()));
    expect(result.kind).toBe("host-capabilities");
    if (result.kind !== "host-capabilities") return;
    expect(result.capabilities.maximumFps).toBe(144);
    expect(result.capabilities.multiMonitor.available).toBe(true);
    expect(result.capabilities.secureAttention.reason).toBe("policy-not-allowed");
  });

  it("extracts a bounded control RTT acknowledgement", () => {
    expect(decodeControlChannelText(JSON.stringify({
      t: "control-rtt-ack",
      requestId: "probe-1",
    }))).toEqual({ kind: "control-rtt-ack", requestId: "probe-1" });
  });

  it("rejects malformed control RTT acknowledgements", () => {
    expect(decodeControlChannelText(JSON.stringify({
      t: "control-rtt-ack",
      requestId: "",
    }))).toEqual({ kind: "invalid" });
    expect(decodeControlChannelText(JSON.stringify({
      t: "control-rtt-ack",
      requestId: "x".repeat(129),
    }))).toEqual({ kind: "invalid" });
  });

  it("leaves unrelated control messages for their own handlers", () => {
    expect(decodeControlChannelText(JSON.stringify({
      t: "monitor-state",
      monitors: [],
    }))).toEqual({ kind: "other" });
  });

  it("rejects malformed and unsupported capability envelopes", () => {
    expect(decodeControlChannelText("not-json")).toEqual({ kind: "invalid" });
    expect(decodeControlChannelText("[]")).toEqual({ kind: "invalid" });
    expect(decodeControlChannelText(JSON.stringify({
      ...nestedCapabilitiesMessage(),
      version: 2,
    }))).toEqual({ kind: "invalid" });
  });

  it("bounds untrusted control channel text before parsing", () => {
    expect(decodeControlChannelText("x".repeat(64 * 1024 + 1))).toEqual({ kind: "invalid" });
  });
});
