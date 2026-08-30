export const INITIAL_VIDEO_JITTER_BUFFER_TARGET_MS = 30;
export const MIN_VIDEO_JITTER_BUFFER_TARGET_MS = 20;
export const MAX_VIDEO_JITTER_BUFFER_TARGET_MS = 120;
export const JITTER_BUFFER_TARGET_DEADBAND_MS = 3;
export const JITTER_BUFFER_RECOVERY_STEP_MS = 20;

function clampJitterBufferTargetMs(value: unknown): number {
  const numeric = typeof value === "number" && Number.isFinite(value)
    ? value
    : INITIAL_VIDEO_JITTER_BUFFER_TARGET_MS;
  return Math.round(Math.max(
    MIN_VIDEO_JITTER_BUFFER_TARGET_MS,
    Math.min(MAX_VIDEO_JITTER_BUFFER_TARGET_MS, numeric),
  ));
}

export function videoJitterBufferTargetMs(
  jitterMs: number | null,
  lossPct: number,
): number {
  const jitter = Number.isFinite(jitterMs) && jitterMs !== null
    ? Math.max(0, Math.min(100, jitterMs))
    : 5;
  const loss = Number.isFinite(lossPct)
    ? Math.max(0, Math.min(100, lossPct))
    : 0;

  // Remote desktop favors freshness over smooth media playback. Keep the base
  // target small, then buy only enough buffer to cover observed network jitter.
  // Loss adds a modest recovery margin without allowing seconds of stale video.
  const lossMargin = loss >= 8 ? 25 : loss >= 3 ? 12 : loss >= 1 ? 5 : 0;
  const target = 20 + (jitter * 1.35) + lossMargin;
  return clampJitterBufferTargetMs(target);
}

export function nextJitterBufferTargetMs(
  currentTargetMs: unknown,
  desiredTargetMs: number,
): number {
  const desired = clampJitterBufferTargetMs(desiredTargetMs);
  if (typeof currentTargetMs !== "number" || !Number.isFinite(currentTargetMs)) {
    return desired;
  }

  const current = clampJitterBufferTargetMs(currentTargetMs);
  const delta = desired - current;
  if (Math.abs(delta) <= JITTER_BUFFER_TARGET_DEADBAND_MS) return current;

  // React quickly when the network gets worse so playout can absorb fresh
  // jitter, but reduce buffering gradually as the link recovers. This avoids
  // one-second telemetry noise from repeatedly changing browser playout.
  if (delta > 0) return desired;
  return Math.max(desired, current - JITTER_BUFFER_RECOVERY_STEP_MS);
}

type JitterBufferReceiver = {
  jitterBufferTarget: number | null;
};

export function trySetJitterBufferTarget(
  receiver: object,
  targetMs: number,
): boolean {
  if (!("jitterBufferTarget" in receiver)) return false;
  try {
    const targetReceiver = receiver as JitterBufferReceiver;
    const nextTarget = nextJitterBufferTargetMs(targetReceiver.jitterBufferTarget, targetMs);
    if (typeof targetReceiver.jitterBufferTarget === "number" &&
        Number.isFinite(targetReceiver.jitterBufferTarget) &&
        Math.round(targetReceiver.jitterBufferTarget) === nextTarget) {
      return true;
    }
    targetReceiver.jitterBufferTarget = nextTarget;
    return true;
  } catch {
    return false;
  }
}
