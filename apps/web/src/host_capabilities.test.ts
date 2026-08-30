import { describe, expect, it } from "vitest";

import { parseHostCapabilitiesMessage } from "./host_capabilities";

describe("HostCapabilitiesV1 parsing", () => {
  it("maps the current flat host-capabilities message without overclaiming new features", () => {
    const parsed = parseHostCapabilitiesMessage({
      t: "host-capabilities",
      version: 1,
      secureAttentionAvailable: false,
      secureAttentionReason: "policy-not-allowed",
      secureAttentionPolicy: "disabled",
      clipboardAvailable: true,
      fileTransferAvailable: true,
      audioAvailable: false,
      protectedDesktopAvailable: false,
    });

    expect(parsed).not.toBeNull();
    expect(parsed?.secureAttention).toEqual({
      available: false,
      reason: "policy-not-allowed",
      metadata: { policy: "disabled" },
    });
    expect(parsed?.clipboard.available).toBe(true);
    expect(parsed?.fileTransfer.available).toBe(true);
    expect(parsed?.systemAudio.available).toBe(false);
    expect(parsed?.multiMonitor).toEqual({
      available: false,
      reason: "legacy-capability-not-advertised",
    });
    expect(parsed?.maximumFps).toBe(0);
  });

  it("parses the versioned nested schema including media limits", () => {
    const parsed = parseHostCapabilitiesMessage({
      t: "host-capabilities",
      version: 1,
      capabilities: {
        version: 1,
        secureAttention: {
          available: true,
          metadata: { policy: "services" },
        },
        clipboard: { available: true },
        fileTransfer: { available: true },
        systemAudio: {
          available: true,
          metadata: { backend: "wasapi-loopback" },
        },
        microphone: { available: false, reason: "not-implemented" },
        protectedDesktop: { available: false, reason: "not-implemented" },
        multiMonitor: { available: true },
        highRefresh: { available: true },
        virtualDisplay: { available: false },
        privacyMode: { available: false },
        virtualHid: { available: false },
        gamepad: { available: false },
        codecs: [{
          codec: "H264",
          profiles: ["baseline", "main"],
          maximumFps: 144,
          maximumResolution: { width: 3840, height: 2160 },
        }],
        maximumFps: 144,
        maximumResolution: { width: 3840, height: 2160 },
      },
    });

    expect(parsed?.systemAudio).toEqual({
      available: true,
      metadata: { backend: "wasapi-loopback" },
    });
    expect(parsed?.codecs).toEqual([{
      codec: "h264",
      profiles: ["baseline", "main"],
      maximumFps: 144,
      maximumResolution: { width: 3840, height: 2160 },
    }]);
    expect(parsed?.maximumFps).toBe(144);
    expect(parsed?.maximumResolution).toEqual({ width: 3840, height: 2160 });
  });

  it("rejects unrelated, future-version and malformed envelopes fail-closed", () => {
    expect(parseHostCapabilitiesMessage(null)).toBeNull();
    expect(parseHostCapabilitiesMessage({ t: "monitor-state", version: 1 })).toBeNull();
    expect(parseHostCapabilitiesMessage({ t: "host-capabilities", version: 2 })).toBeNull();
    expect(parseHostCapabilitiesMessage({ t: "host-capabilities", version: "1" })).toBeNull();
  });

  it("bounds untrusted codec and dimension metadata", () => {
    const parsed = parseHostCapabilitiesMessage({
      t: "host-capabilities",
      version: 1,
      capabilities: {
        version: 1,
        secureAttention: { available: false },
        codecs: [
          { codec: "x".repeat(100), maximumFps: 999999 },
          { codec: "H264", maximumResolution: { width: 999999, height: -1 } },
        ],
        maximumFps: 999999,
        maximumResolution: { width: 999999, height: 1080 },
      },
    });

    expect(parsed?.codecs).toEqual([{ codec: "h264" }]);
    expect(parsed?.maximumFps).toBe(0);
    expect(parsed?.maximumResolution).toEqual({ width: 0, height: 1080 });
  });
});
