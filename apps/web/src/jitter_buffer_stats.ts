export type JitterBufferCumulativeStats = {
  emittedCount: number;
  delaySeconds: number;
  targetDelaySeconds: number;
  minimumDelaySeconds: number;
};

export type JitterBufferIntervalMetrics = {
  actualMs: number;
  targetMs: number;
  minimumMs: number;
};

function finiteNonNegative(value: unknown): number {
  return typeof value === "number" && Number.isFinite(value) && value >= 0 ? value : 0;
}

export function readJitterBufferCumulativeStats(
  inboundVideo: Record<string, unknown>,
): JitterBufferCumulativeStats {
  return {
    emittedCount: finiteNonNegative(inboundVideo.jitterBufferEmittedCount),
    delaySeconds: finiteNonNegative(inboundVideo.jitterBufferDelay),
    targetDelaySeconds: finiteNonNegative(inboundVideo.jitterBufferTargetDelay),
    minimumDelaySeconds: finiteNonNegative(inboundVideo.jitterBufferMinimumDelay),
  };
}

export function jitterBufferIntervalMetrics(
  previous: JitterBufferCumulativeStats,
  current: JitterBufferCumulativeStats,
): JitterBufferIntervalMetrics | null {
  const emitted = current.emittedCount - previous.emittedCount;
  if (!Number.isFinite(emitted) || emitted <= 0) return null;

  // WebRTC cumulative stats can reset when a receiver/track is replaced. Never
  // turn that reset into an artificial 0 ms latency sample; wait for the next
  // monotonic interval instead.
  if (current.delaySeconds < previous.delaySeconds ||
      current.targetDelaySeconds < previous.targetDelaySeconds ||
      current.minimumDelaySeconds < previous.minimumDelaySeconds) {
    return null;
  }

  const averageMs = (currentValue: number, previousValue: number) =>
    ((currentValue - previousValue) / emitted) * 1000;

  return {
    actualMs: averageMs(current.delaySeconds, previous.delaySeconds),
    targetMs: averageMs(current.targetDelaySeconds, previous.targetDelaySeconds),
    minimumMs: averageMs(current.minimumDelaySeconds, previous.minimumDelaySeconds),
  };
}
