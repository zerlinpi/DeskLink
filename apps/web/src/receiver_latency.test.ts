import { describe, expect, it } from "vitest";

import {
  INITIAL_VIDEO_JITTER_BUFFER_TARGET_MS,
  JITTER_BUFFER_RECOVERY_STEP_MS,
  JITTER_BUFFER_TARGET_DEADBAND_MS,
  MAX_VIDEO_JITTER_BUFFER_TARGET_MS,
  MIN_VIDEO_JITTER_BUFFER_TARGET_MS,
  nextJitterBufferTargetMs,
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

  it("raises buffering immediately but lowers it gradually", () => {
    expect(nextJitterBufferTargetMs(30, 80)).toBe(80);
    expect(nextJitterBufferTargetMs(100, 20)).toBe(100 - JITTER_BUFFER_RECOVERY_STEP_MS);
    expect(nextJitterBufferTargetMs(40, 20)).toBe(20);
  });

  it("uses a small deadband to avoid one-second target churn", () => {
    expect(nextJitterBufferTargetMs(40, 40 + JITTER_BUFFER_TARGET_DEADBAND_MS)).toBe(40);
    expect(nextJitterBufferTargetMs(40, 40 - JITTER_BUFFER_TARGET_DEADBAND_MS)).toBe(40);
    expect(nextJitterBufferTargetMs(null, 27)).toBe(27);
    expect(nextJitterBufferTargetMs(Number.NaN, 27)).toBe(27);
  });

  it("feature-detects receiver support, bounds assignments, and applies recovery hysteresis", () => {
    const supported = { jitterBufferTarget: null as number | null };
    expect(trySetJitterBufferTarget(supported, 1)).toBe(true);
    expect(supported.jitterBufferTarget).toBe(MIN_VIDEO_JITTER_BUFFER_TARGET_MS);

    supported.jitterBufferTarget = 100;
    expect(trySetJitterBufferTarget(supported, 20)).toBe(true);
    expect(supported.jitterBufferTarget).toBe(100 - JITTER_BUFFER_RECOVERY_STEP_MS);

    expect(trySetJitterBufferTarget({}, 30)).toBe(false);

    const throwing = Object.defineProperty({}, "jitterBufferTarget", {
      set() {
        throw new RangeError("unsupported target");
      },
    });
    expect(trySetJitterBufferTarget(throwing, 30)).toBe(false);
  });
});
