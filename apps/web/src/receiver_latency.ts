export const INITIAL_VIDEO_JITTER_BUFFER_TARGET_MS = 30;
export const MIN_VIDEO_JITTER_BUFFER_TARGET_MS = 20;
export const MAX_VIDEO_JITTER_BUFFER_TARGET_MS = 120;

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
  return Math.round(Math.max(
    MIN_VIDEO_JITTER_BUFFER_TARGET_MS,
    Math.min(MAX_VIDEO_JITTER_BUFFER_TARGET_MS, target),
  ));
}

type JitterBufferReceiver = {
  jitterBufferTarget: number | null;
};

export function trySetJitterBufferTarget(
  receiver: object,
  targetMs: number,
): boolean {
  if (!("jitterBufferTarget" in receiver)) return false;
  const normalizedTarget = Math.max(
    MIN_VIDEO_JITTER_BUFFER_TARGET_MS,
    Math.min(MAX_VIDEO_JITTER_BUFFER_TARGET_MS, Math.round(targetMs)),
  );
  try {
    (receiver as JitterBufferReceiver).jitterBufferTarget = normalizedTarget;
    return true;
  } catch {
    return false;
  }
}
