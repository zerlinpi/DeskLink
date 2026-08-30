import { describe, expect, it } from "vitest";

import {
  DATA_CHANNEL_OPEN_TIMEOUT_MS,
  DATA_CHANNEL_RECOVERY_BASE_DELAY_MS,
  DATA_CHANNEL_RECOVERY_MAX_DELAY_MS,
  dataChannelRecoveryDelayMs,
  resetDataChannelRecoveryAttempt,
  shouldArmDataChannelOpenWatchdog,
  shouldExpireDataChannelOpenWatchdog,
  shouldScheduleDataChannelRecovery,
} from "./data_channel_recovery";

const recoverable = {
  manualDisconnect: false,
  currentPeer: true,
  peerState: "connected" as RTCPeerConnectionState,
  channelCurrent: true,
  channelState: "closed" as RTCDataChannelState,
  replacementScheduled: false,
};

const opening = {
  manualDisconnect: false,
  currentPeer: true,
  peerState: "connected" as RTCPeerConnectionState,
  channelCurrent: true,
  channelState: "connecting" as RTCDataChannelState,
  watchdogScheduled: false,
};

describe("shouldScheduleDataChannelRecovery", () => {
  it("recovers a closed current channel while the peer is otherwise connected", () => {
    expect(shouldScheduleDataChannelRecovery(recoverable)).toBe(true);
  });

  it("does not recover during manual disconnect or for stale session resources", () => {
    expect(shouldScheduleDataChannelRecovery({ ...recoverable, manualDisconnect: true })).toBe(false);
    expect(shouldScheduleDataChannelRecovery({ ...recoverable, currentPeer: false })).toBe(false);
    expect(shouldScheduleDataChannelRecovery({ ...recoverable, channelCurrent: false })).toBe(false);
  });

  it("leaves peer-level failure to the existing ICE/peer recovery path", () => {
    for (const state of ["new", "connecting", "disconnected", "failed", "closed"] as RTCPeerConnectionState[]) {
      expect(shouldScheduleDataChannelRecovery({ ...recoverable, peerState: state })).toBe(false);
    }
  });

  it("does not replace channels that are still opening/open or already scheduled", () => {
    for (const state of ["connecting", "open", "closing"] as RTCDataChannelState[]) {
      expect(shouldScheduleDataChannelRecovery({ ...recoverable, channelState: state })).toBe(false);
    }
    expect(shouldScheduleDataChannelRecovery({ ...recoverable, replacementScheduled: true })).toBe(false);
  });
});

describe("DataChannel open watchdog", () => {
  it("arms only for the current connecting channel on a connected peer", () => {
    expect(shouldArmDataChannelOpenWatchdog(opening)).toBe(true);
    expect(shouldArmDataChannelOpenWatchdog({ ...opening, watchdogScheduled: true })).toBe(false);
    expect(shouldArmDataChannelOpenWatchdog({ ...opening, manualDisconnect: true })).toBe(false);
    expect(shouldArmDataChannelOpenWatchdog({ ...opening, currentPeer: false })).toBe(false);
    expect(shouldArmDataChannelOpenWatchdog({ ...opening, channelCurrent: false })).toBe(false);
    expect(shouldArmDataChannelOpenWatchdog({ ...opening, channelState: "open" })).toBe(false);
    expect(shouldArmDataChannelOpenWatchdog({ ...opening, peerState: "disconnected" })).toBe(false);
  });

  it("expires only while the same channel is still stalled", () => {
    const expirationGate = {
      manualDisconnect: false,
      currentPeer: true,
      peerState: "connected" as RTCPeerConnectionState,
      channelCurrent: true,
      channelState: "connecting" as RTCDataChannelState,
    };
    expect(shouldExpireDataChannelOpenWatchdog(expirationGate)).toBe(true);
    expect(shouldExpireDataChannelOpenWatchdog({ ...expirationGate, channelState: "open" })).toBe(false);
    expect(shouldExpireDataChannelOpenWatchdog({ ...expirationGate, channelCurrent: false })).toBe(false);
    expect(shouldExpireDataChannelOpenWatchdog({ ...expirationGate, peerState: "failed" })).toBe(false);
    expect(shouldExpireDataChannelOpenWatchdog({ ...expirationGate, manualDisconnect: true })).toBe(false);
  });

  it("keeps the timeout bounded for low-latency recovery", () => {
    expect(DATA_CHANNEL_OPEN_TIMEOUT_MS).toBeGreaterThanOrEqual(1_000);
    expect(DATA_CHANNEL_OPEN_TIMEOUT_MS).toBeLessThanOrEqual(10_000);
  });
});

describe("dataChannelRecoveryDelayMs", () => {
  it("uses bounded exponential backoff for repeated SCTP/channel failures", () => {
    expect([0, 1, 2, 3, 4, 5].map(dataChannelRecoveryDelayMs)).toEqual([
      250,
      500,
      1_000,
      2_000,
      4_000,
      4_000,
    ]);
    expect(dataChannelRecoveryDelayMs(100)).toBe(DATA_CHANNEL_RECOVERY_MAX_DELAY_MS);
    expect(dataChannelRecoveryDelayMs(-1)).toBe(DATA_CHANNEL_RECOVERY_BASE_DELAY_MS);
    expect(dataChannelRecoveryDelayMs(Number.NaN)).toBe(DATA_CHANNEL_RECOVERY_BASE_DELAY_MS);
  });

  it("resets only the channel that successfully reopened", () => {
    expect(resetDataChannelRecoveryAttempt("control", { control: 4, pointer: 2 })).toEqual({
      control: 0,
      pointer: 2,
    });
    expect(resetDataChannelRecoveryAttempt("pointer", { control: 4, pointer: 2 })).toEqual({
      control: 4,
      pointer: 0,
    });
  });
});
