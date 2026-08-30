import {
  jitterBufferIntervalMetrics,
  readJitterBufferCumulativeStats,
  type JitterBufferCumulativeStats,
  type JitterBufferIntervalMetrics,
} from "./jitter_buffer_stats";

export const MIN_VIDEO_TELEMETRY_INTERVAL_MS = 250;

export type VideoTelemetryBaseline = {
  atMs: number;
  packetsReceived: number;
  packetsLost: number;
  framesDecoded: number;
  framesDropped: number;
  jitterBuffer: JitterBufferCumulativeStats;
};

export type VideoTelemetryInterval = {
  elapsedSeconds: number;
  lossPct: number;
  decodeFps: number;
  droppedFps: number;
  jitterBuffer: JitterBufferIntervalMetrics | null;
};

function finiteNonNegative(value: unknown): number {
  return typeof value === "number" && Number.isFinite(value) && value >= 0 ? value : 0;
}

export function readVideoTelemetryBaseline(
  inboundVideo: Record<string, unknown>,
  atMs: number,
): VideoTelemetryBaseline {
  return {
    atMs: finiteNonNegative(atMs),
    packetsReceived: finiteNonNegative(inboundVideo.packetsReceived),
    packetsLost: finiteNonNegative(inboundVideo.packetsLost),
    framesDecoded: finiteNonNegative(inboundVideo.framesDecoded),
    framesDropped: finiteNonNegative(inboundVideo.framesDropped),
    jitterBuffer: readJitterBufferCumulativeStats(inboundVideo),
  };
}

export function videoTelemetryInterval(
  previous: VideoTelemetryBaseline,
  current: VideoTelemetryBaseline,
): VideoTelemetryInterval | null {
  const elapsedMs = current.atMs - previous.atMs;
  if (!Number.isFinite(elapsedMs) || elapsedMs < MIN_VIDEO_TELEMETRY_INTERVAL_MS) return null;

  // All of these RTCInboundRtpStreamStats fields are cumulative. A decrease
  // means the receiver/track/stat object changed, so use the new sample as the
  // next baseline instead of inventing a clean 0-loss/0-latency interval.
  if (current.packetsReceived < previous.packetsReceived ||
      current.packetsLost < previous.packetsLost ||
      current.framesDecoded < previous.framesDecoded ||
      current.framesDropped < previous.framesDropped) {
    return null;
  }

  const elapsedSeconds = elapsedMs / 1000;
  const receivedDelta = current.packetsReceived - previous.packetsReceived;
  const lostDelta = current.packetsLost - previous.packetsLost;
  const packetDelta = receivedDelta + lostDelta;

  return {
    elapsedSeconds,
    lossPct: packetDelta > 0 ? (lostDelta / packetDelta) * 100 : 0,
    decodeFps: (current.framesDecoded - previous.framesDecoded) / elapsedSeconds,
    droppedFps: (current.framesDropped - previous.framesDropped) / elapsedSeconds,
    jitterBuffer: jitterBufferIntervalMetrics(previous.jitterBuffer, current.jitterBuffer),
  };
}
