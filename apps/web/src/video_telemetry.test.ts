import { describe, expect, it } from "vitest";

import {
  MIN_VIDEO_TELEMETRY_INTERVAL_MS,
  readVideoTelemetryBaseline,
  videoTelemetryInterval,
} from "./video_telemetry";

function sample(overrides: Record<string, unknown> = {}) {
  return {
    packetsReceived: 1_000,
    packetsLost: 10,
    framesDecoded: 600,
    framesDropped: 5,
    jitterBufferEmittedCount: 600,
    jitterBufferDelay: 12,
    jitterBufferTargetDelay: 18,
    jitterBufferMinimumDelay: 9,
    ...overrides,
  };
}

describe("readVideoTelemetryBaseline", () => {
  it("captures cumulative RTP/video/jitter counters", () => {
    expect(readVideoTelemetryBaseline(sample(), 1_000)).toEqual({
      atMs: 1_000,
      packetsReceived: 1_000,
      packetsLost: 10,
      framesDecoded: 600,
      framesDropped: 5,
      jitterBuffer: {
        emittedCount: 600,
        delaySeconds: 12,
        targetDelaySeconds: 18,
        minimumDelaySeconds: 9,
      },
    });
  });

  it("normalizes invalid counters without emitting negative telemetry", () => {
    const baseline = readVideoTelemetryBaseline(sample({
      packetsReceived: Number.NaN,
      packetsLost: -2,
      framesDecoded: Number.POSITIVE_INFINITY,
      framesDropped: undefined,
    }), -100);
    expect(baseline.atMs).toBe(0);
    expect(baseline.packetsReceived).toBe(0);
    expect(baseline.packetsLost).toBe(0);
    expect(baseline.framesDecoded).toBe(0);
    expect(baseline.framesDropped).toBe(0);
  });
});

describe("videoTelemetryInterval", () => {
  it("computes packet loss, decode/drop FPS, and jitter-buffer delay per frame", () => {
    const previous = readVideoTelemetryBaseline(sample(), 1_000);
    const current = readVideoTelemetryBaseline(sample({
      packetsReceived: 1_190,
      packetsLost: 20,
      framesDecoded: 720,
      framesDropped: 9,
      jitterBufferEmittedCount: 720,
      jitterBufferDelay: 15.6,
      jitterBufferTargetDelay: 22.8,
      jitterBufferMinimumDelay: 11.4,
    }), 3_000);

    const interval = videoTelemetryInterval(previous, current);
    expect(interval).not.toBeNull();
    expect(interval?.elapsedSeconds).toBe(2);
    expect(interval?.lossPct).toBeCloseTo(5, 6);
    expect(interval?.decodeFps).toBeCloseTo(60, 6);
    expect(interval?.droppedFps).toBeCloseTo(2, 6);
    expect(interval?.jitterBuffer?.actualMs).toBeCloseTo(30, 6);
    expect(interval?.jitterBuffer?.targetMs).toBeCloseTo(40, 6);
    expect(interval?.jitterBuffer?.minimumMs).toBeCloseTo(20, 6);
  });

  it("rejects undersized intervals instead of distorting the elapsed time", () => {
    const previous = readVideoTelemetryBaseline(sample(), 1_000);
    const shortInterval = readVideoTelemetryBaseline(sample({ framesDecoded: 610 }), 1_100);
    expect(videoTelemetryInterval(previous, shortInterval)).toBeNull();

    const boundary = readVideoTelemetryBaseline(sample({
      framesDecoded: 615,
      jitterBufferEmittedCount: 615,
      jitterBufferDelay: 12.45,
      jitterBufferTargetDelay: 18.6,
      jitterBufferMinimumDelay: 9.3,
    }), 1_000 + MIN_VIDEO_TELEMETRY_INTERVAL_MS);
    expect(videoTelemetryInterval(previous, boundary)?.decodeFps).toBe(60);
  });

  it("fails closed when RTP or frame counters reset", () => {
    const previous = readVideoTelemetryBaseline(sample(), 1_000);
    for (const reset of [
      { packetsReceived: 1 },
      { packetsLost: 1 },
      { framesDecoded: 1 },
      { framesDropped: 1 },
    ]) {
      expect(videoTelemetryInterval(
        previous,
        readVideoTelemetryBaseline(sample(reset), 2_000),
      )).toBeNull();
    }
  });

  it("rejects stale/non-forward timestamps", () => {
    const previous = readVideoTelemetryBaseline(sample(), 1_000);
    expect(videoTelemetryInterval(previous, readVideoTelemetryBaseline(sample(), 1_000))).toBeNull();
    expect(videoTelemetryInterval(previous, readVideoTelemetryBaseline(sample(), 900))).toBeNull();
  });
});
