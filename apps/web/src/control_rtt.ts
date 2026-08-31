export const CONTROL_RTT_PROBE_TIMEOUT_MS = 5_000;
export const MAX_CONTROL_RTT_REQUEST_ID_BYTES = 128;

export type PendingControlRttProbe = {
  requestId: string;
  sentAtMs: number;
};

export function isValidControlRttRequestId(value: unknown): value is string {
  return typeof value === "string" &&
    value.length > 0 &&
    value.length <= MAX_CONTROL_RTT_REQUEST_ID_BYTES;
}

export function shouldIssueControlRttProbe(
  pending: PendingControlRttProbe | null,
  nowMs: number,
): boolean {
  if (!Number.isFinite(nowMs)) return false;
  if (!pending) return true;
  if (!isValidControlRttRequestId(pending.requestId) || !Number.isFinite(pending.sentAtMs)) return true;
  return nowMs - pending.sentAtMs >= CONTROL_RTT_PROBE_TIMEOUT_MS;
}

export function resolveControlRttAck(
  pending: PendingControlRttProbe | null,
  requestId: unknown,
  nowMs: number,
): number | null {
  if (!pending || !isValidControlRttRequestId(requestId) || requestId !== pending.requestId) return null;
  if (!Number.isFinite(nowMs) || !Number.isFinite(pending.sentAtMs)) return null;
  const elapsed = nowMs - pending.sentAtMs;
  if (elapsed < 0 || elapsed > CONTROL_RTT_PROBE_TIMEOUT_MS) return null;
  return elapsed;
}
