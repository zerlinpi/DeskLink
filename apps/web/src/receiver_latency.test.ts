import { describe, expect, it } from "vitest";

import {
  INITIAL_VIDEO_JITTER_BUFFER_TARGET_MS,
  MAX_VIDEO_JITTER_BUFFER_TARGET_MS,
  MIN_VIDEO_JITTER_BUFFER_TARGET_MS,
  trySetJitterBufferTarget,
  videoJitterBufferTargetMs,
} from "./receiver_latency";

describe("video jitter buffer policy", () => {
  it("keeps healthy links close to the low-latency floor", () => {
    expect(videoJitterBufferTargetMs(0, 0)).toBe(20);
    expect(videoJitterBufferTargetMs(5, 0)).toBeLessThanOrEqual(INITIAL_VIDEO_JITTER_BUFFER_TARGET_MS);
    expect(videoJitterBufferTargetMs(10, 0)).toBeLessThan(40);
  });

  it("adds bounded headroom as jitter and loss increase", () => {
    const healthy = videoJitterBufferTargetMs(5, 0.2);
    const moderate = videoJitterBufferTargetMs(30, 3.5);
    const severe = videoJitterBufferTargetMs(90, 10);
    expect(moderate).toBeGreaterThan(healthy);
    expect(severe).toBeGreaterThan(moderate);
    expect(severe).toBeLessThanOrEqual(MAX_VIDEO_JITTER_BUFFER_TARGET_MS);
  });

  it("normalizes invalid telemetry conservatively", () => {
    expect(videoJitterBufferTargetMs(null, Number.NaN)).toBeGreaterThanOrEqual(MIN_VIDEO_JITTER_BUFFER_TARGET_MS);
    expect(videoJitterBufferTargetMs(Number.POSITIVE_INFINITY, -10)).toBeGreaterThanOrEqual(MIN_VIDEO_JITTER_BUFFER_TARGET_MS);
  });

  it("feature-detects receiver support and bounds assignments", () => {
    const supported = { jitterBufferTarget: null as number | null };
    expect(trySetJitterBufferTarget(supported, 1)).toBe(true);
    expect(supported.jitterBufferTarget).toBe(MIN_VIDEO_JITTER_BUFFER_TARGET_MS);

    expect(trySetJitterBufferTarget({}, 30)).toBe(false);

    const throwing = Object.defineProperty({}, "jitterBufferTarget", {
      set() {
        throw new RangeError("unsupported target");
      },
    });
    expect(trySetJitterBufferTarget(throwing, 30)).toBe(false);
  });
});
