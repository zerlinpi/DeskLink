export function isWakeLockSessionStatusActive(status: string): boolean {
  const value = status.trim().toLowerCase();
  if (["new", "connecting", "connected", "disconnected", "checking"].includes(value)) return true;
  return [
    "signaling",
    "authorizing",
    "proving host access",
    "negotiating",
    "reconnecting",
    "control ready",
    "host offline",
  ].some((marker) => value.includes(marker));
}

export function shouldHoldWakeLock(status: string, visibilityState: DocumentVisibilityState): boolean {
  return visibilityState === "visible" && isWakeLockSessionStatusActive(status);
}

export function isWakeLockRequestCurrent(
  requestGeneration: number,
  currentGeneration: number,
  status: string,
  visibilityState: DocumentVisibilityState,
): boolean {
  return requestGeneration === currentGeneration && shouldHoldWakeLock(status, visibilityState);
}
