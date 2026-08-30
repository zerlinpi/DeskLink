import { describe, expect, it } from "vitest";

import {
  jitterBufferIntervalMetrics,
  readJitterBufferCumulativeStats,
} from "./jitter_buffer_stats";

describe("readJitterBufferCumulativeStats", () => {
  it("reads finite cumulative WebRTC jitter-buffer counters", () => {
    expect(readJitterBufferCumulativeStats({
      jitterBufferEmittedCount: 120,
      jitterBufferDelay: 2.4,
      jitterBufferTargetDelay: 3.6,
      jitterBufferMinimumDelay: 1.8,
    })).toEqual({
      emittedCount: 120,
      delaySeconds: 2.4,
      targetDelaySeconds: 3.6,
      minimumDelaySeconds: 1.8,
    });
  });

  it("normalizes missing, negative, and non-finite counters", () => {
    expect(readJitterBufferCumulativeStats({
      jitterBufferEmittedCount: Number.NaN,
      jitterBufferDelay: -1,
      jitterBufferTargetDelay: Number.POSITIVE_INFINITY,
      jitterBufferMinimumDelay: undefined,
    })).toEqual({
      emittedCount: 0,
      delaySeconds: 0,
      targetDelaySeconds: 0,
      minimumDelaySeconds: 0,
    });
  });
});

describe("jitterBufferIntervalMetrics", () => {
  it("converts cumulative delay counters into per-frame interval latency", () => {
    const metrics = jitterBufferIntervalMetrics(
      {
        emittedCount: 100,
        delaySeconds: 2,
        targetDelaySeconds: 3,
        minimumDelaySeconds: 1,
      },
      {
        emittedCount: 120,
        delaySeconds: 2.6,
        targetDelaySeconds: 3.8,
        minimumDelaySeconds: 1.4,
      },
    );

    expect(metrics).not.toBeNull();
    expect(metrics?.actualMs).toBeCloseTo(30, 6);
    expect(metrics?.targetMs).toBeCloseTo(40, 6);
    expect(metrics?.minimumMs).toBeCloseTo(20, 6);
  });

  it("waits for at least one newly emitted frame", () => {
    expect(jitterBufferIntervalMetrics(
      { emittedCount: 10, delaySeconds: 1, targetDelaySeconds: 1, minimumDelaySeconds: 1 },
      { emittedCount: 10, delaySeconds: 1.1, targetDelaySeconds: 1.1, minimumDelaySeconds: 1.1 },
    )).toBeNull();
  });

  it("fails closed when WebRTC cumulative counters reset", () => {
    expect(jitterBufferIntervalMetrics(
      { emittedCount: 100, delaySeconds: 5, targetDelaySeconds: 6, minimumDelaySeconds: 4 },
      { emittedCount: 120, delaySeconds: 1, targetDelaySeconds: 2, minimumDelaySeconds: 1 },
    )).toBeNull();

    expect(jitterBufferIntervalMetrics(
      { emittedCount: 100, delaySeconds: 5, targetDelaySeconds: 6, minimumDelaySeconds: 4 },
      { emittedCount: 120, delaySeconds: 5.5, targetDelaySeconds: 5, minimumDelaySeconds: 4.2 },
    )).toBeNull();
  });
});
