import { describe, expect, it } from "vitest";

import fixture from "./fixtures/host-capabilities-v1.windows.json";
import { parseHostCapabilitiesMessage } from "./host_capabilities";

describe("HostCapabilitiesV1 producer/consumer contract", () => {
  it("parses the canonical Windows producer fixture without legacy fallback", () => {
    expect("capabilities" in fixture).toBe(true);
    expect("secureAttentionAvailable" in fixture).toBe(false);

    const parsed = parseHostCapabilitiesMessage(fixture);
    expect(parsed).not.toBeNull();
    expect(parsed?.version).toBe(1);
    expect(parsed?.secureAttention.available).toBe(true);
    expect(parsed?.secureAttention.metadata?.policy).toBe("allow-services");
    expect(parsed?.clipboard.available).toBe(true);
    expect(parsed?.fileTransfer.available).toBe(true);
    expect(parsed?.systemAudio).toEqual({ available: false, reason: "not-implemented" });
    expect(parsed?.protectedDesktop).toEqual({ available: false, reason: "not-implemented" });
    expect(parsed?.multiMonitor.available).toBe(true);
    expect(parsed?.highRefresh.available).toBe(true);
    expect(parsed?.codecs).toEqual([
      {
        codec: "h264",
        maximumFps: 144,
        maximumResolution: { width: 1920, height: 1080 },
      },
    ]);
    expect(parsed?.maximumFps).toBe(144);
    expect(parsed?.maximumResolution).toEqual({ width: 1920, height: 1080 });
  });
});
