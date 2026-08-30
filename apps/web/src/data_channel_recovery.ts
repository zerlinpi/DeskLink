export type RecoverableChannelKind = "control" | "pointer";

export type DataChannelRecoveryGate = {
  manualDisconnect: boolean;
  currentPeer: boolean;
  peerState: RTCPeerConnectionState;
  channelCurrent: boolean;
  channelState: RTCDataChannelState;
  replacementScheduled: boolean;
};

export type DataChannelOpenWatchdogGate = {
  manualDisconnect: boolean;
  currentPeer: boolean;
  peerState: RTCPeerConnectionState;
  channelCurrent: boolean;
  channelState: RTCDataChannelState;
  watchdogScheduled: boolean;
};

export const DATA_CHANNEL_RECOVERY_BASE_DELAY_MS = 250;
export const DATA_CHANNEL_RECOVERY_MAX_DELAY_MS = 4_000;
export const DATA_CHANNEL_OPEN_TIMEOUT_MS = 8_000;

export function shouldScheduleDataChannelRecovery(
  gate: DataChannelRecoveryGate,
): boolean {
  return !gate.manualDisconnect &&
    gate.currentPeer &&
    gate.peerState === "connected" &&
    gate.channelCurrent &&
    gate.channelState === "closed" &&
    !gate.replacementScheduled;
}

export function shouldArmDataChannelOpenWatchdog(
  gate: DataChannelOpenWatchdogGate,
): boolean {
  return !gate.manualDisconnect &&
    gate.currentPeer &&
    gate.peerState === "connected" &&
    gate.channelCurrent &&
    gate.channelState === "connecting" &&
    !gate.watchdogScheduled;
}

export function shouldExpireDataChannelOpenWatchdog(
  gate: Omit<DataChannelOpenWatchdogGate, "watchdogScheduled">,
): boolean {
  return !gate.manualDisconnect &&
    gate.currentPeer &&
    gate.peerState === "connected" &&
    gate.channelCurrent &&
    gate.channelState === "connecting";
}

export function dataChannelRecoveryDelayMs(attempt: number): number {
  const normalizedAttempt = Number.isFinite(attempt)
    ? Math.max(0, Math.floor(attempt))
    : 0;
  return Math.min(
    DATA_CHANNEL_RECOVERY_MAX_DELAY_MS,
    DATA_CHANNEL_RECOVERY_BASE_DELAY_MS * (2 ** Math.min(normalizedAttempt, 8)),
  );
}

export function resetDataChannelRecoveryAttempt(
  kind: RecoverableChannelKind,
  attempts: Record<RecoverableChannelKind, number>,
): Record<RecoverableChannelKind, number> {
  return {
    ...attempts,
    [kind]: 0,
  };
}
