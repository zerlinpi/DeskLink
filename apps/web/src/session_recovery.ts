export type PeerConnectionState = RTCPeerConnectionState;

export type IceRestartGate = {
  manualDisconnect: boolean;
  currentPeer: boolean;
  restartInFlight: boolean;
  timerScheduled: boolean;
  connectionState: PeerConnectionState;
};

export type SignalOpenGate = {
  manualDisconnect: boolean;
  openInFlight: boolean;
  socketActive: boolean;
};

export type SignalReconnectGate = SignalOpenGate & {
  timerScheduled: boolean;
};

export type SignalOpenScope = {
  manualDisconnect: boolean;
  expectedSession: string;
  currentSession: string;
  expectedTarget: string;
  currentTarget: string;
};

export type SignalCallbackScope = SignalOpenScope & {
  sourceSocketCurrent: boolean;
};

const SIGNAL_RECONNECT_BASE_MS = 500;
const SIGNAL_RECONNECT_MAX_MS = 10_000;
const SIGNAL_RECONNECT_EXPONENT_CAP = 5;

export const HOST_WAIT_REFRESH_SAFETY_MS = 5_000;
export const HOST_WAIT_REFRESH_MAX_MS = 10 * 60 * 1_000;

export function signalReconnectDelayMs(attempt: number): number {
  const normalizedAttempt = Number.isFinite(attempt) ? Math.max(0, Math.trunc(attempt)) : 0;
  return Math.min(
    SIGNAL_RECONNECT_MAX_MS,
    SIGNAL_RECONNECT_BASE_MS * (2 ** Math.min(normalizedAttempt, SIGNAL_RECONNECT_EXPONENT_CAP)),
  );
}

export function shouldBeginSignalOpen(gate: SignalOpenGate): boolean {
  return !gate.manualDisconnect && !gate.openInFlight && !gate.socketActive;
}

export function isSignalOpenScopeCurrent(scope: SignalOpenScope): boolean {
  return !scope.manualDisconnect &&
    scope.expectedSession === scope.currentSession &&
    scope.expectedTarget === scope.currentTarget;
}

export function isSignalCallbackScopeCurrent(scope: SignalCallbackScope): boolean {
  return scope.sourceSocketCurrent && isSignalOpenScopeCurrent(scope);
}

export function shouldScheduleSignalReconnect(gate: SignalReconnectGate): boolean {
  return shouldBeginSignalOpen(gate) && !gate.timerScheduled;
}

export function hostWaitRefreshDelayMs(expiresInMs: unknown): number | null {
  const advertised = Number(expiresInMs);
  if (!Number.isFinite(advertised) || advertised <= 0) return null;

  const cappedLifetime = Math.min(HOST_WAIT_REFRESH_MAX_MS, advertised);
  return Math.max(1_000, cappedLifetime - HOST_WAIT_REFRESH_SAFETY_MS);
}

export function shouldScheduleIceRestart(gate: IceRestartGate): boolean {
  if (gate.manualDisconnect || !gate.currentPeer) return false;
  if (gate.restartInFlight || gate.timerScheduled) return false;
  return gate.connectionState !== "connected" && gate.connectionState !== "closed";
}

export function iceRestartDelayMs(immediate: boolean): number {
  return immediate ? 0 : 2_000;
}
