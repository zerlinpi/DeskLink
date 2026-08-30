import { describe, expect, it } from "vitest";
import {
  HOST_WAIT_REFRESH_MAX_MS,
  hostWaitRefreshDelayMs,
  iceRestartDelayMs,
  isSignalCallbackScopeCurrent,
  isSignalOpenScopeCurrent,
  shouldBeginSignalOpen,
  shouldScheduleIceRestart,
  shouldScheduleSignalReconnect,
  signalReconnectDelayMs,
} from "./session_recovery";

describe("signalReconnectDelayMs", () => {
  it("uses bounded exponential backoff", () => {
    expect([0, 1, 2, 3, 4, 5, 6, 20].map(signalReconnectDelayMs)).toEqual([
      500,
      1_000,
      2_000,
      4_000,
      8_000,
      10_000,
      10_000,
      10_000,
    ]);
  });

  it("normalizes invalid or negative attempts", () => {
    expect(signalReconnectDelayMs(-3)).toBe(500);
    expect(signalReconnectDelayMs(Number.NaN)).toBe(500);
    expect(signalReconnectDelayMs(2.9)).toBe(2_000);
  });
});

describe("signal open/reconnect gates", () => {
  const openBase = {
    manualDisconnect: false,
    openInFlight: false,
    socketActive: false,
  } as const;

  it("allows exactly one signal open attempt when idle", () => {
    expect(shouldBeginSignalOpen(openBase)).toBe(true);
    expect(shouldBeginSignalOpen({ ...openBase, openInFlight: true })).toBe(false);
    expect(shouldBeginSignalOpen({ ...openBase, socketActive: true })).toBe(false);
    expect(shouldBeginSignalOpen({ ...openBase, manualDisconnect: true })).toBe(false);
  });

  it("rejects stale async opens after disconnect, session rotation, or target change", () => {
    const current = {
      manualDisconnect: false,
      expectedSession: "session-a",
      currentSession: "session-a",
      expectedTarget: "host-a",
      currentTarget: "host-a",
    } as const;
    expect(isSignalOpenScopeCurrent(current)).toBe(true);
    expect(isSignalOpenScopeCurrent({ ...current, manualDisconnect: true })).toBe(false);
    expect(isSignalOpenScopeCurrent({ ...current, currentSession: "session-b" })).toBe(false);
    expect(isSignalOpenScopeCurrent({ ...current, currentTarget: "host-b" })).toBe(false);
  });

  it("rejects async signal callbacks after their source socket is replaced", () => {
    const current = {
      manualDisconnect: false,
      expectedSession: "session-a",
      currentSession: "session-a",
      expectedTarget: "host-a",
      currentTarget: "host-a",
      sourceSocketCurrent: true,
    } as const;
    expect(isSignalCallbackScopeCurrent(current)).toBe(true);
    expect(isSignalCallbackScopeCurrent({ ...current, sourceSocketCurrent: false })).toBe(false);
    expect(isSignalCallbackScopeCurrent({ ...current, currentSession: "session-b" })).toBe(false);
    expect(isSignalCallbackScopeCurrent({ ...current, currentTarget: "host-b" })).toBe(false);
    expect(isSignalCallbackScopeCurrent({ ...current, manualDisconnect: true })).toBe(false);
  });

  it("does not schedule a second reconnect while opening or while a timer/socket is active", () => {
    expect(shouldScheduleSignalReconnect({ ...openBase, timerScheduled: false })).toBe(true);
    expect(shouldScheduleSignalReconnect({ ...openBase, timerScheduled: true })).toBe(false);
    expect(shouldScheduleSignalReconnect({ ...openBase, openInFlight: true, timerScheduled: false })).toBe(false);
    expect(shouldScheduleSignalReconnect({ ...openBase, socketActive: true, timerScheduled: false })).toBe(false);
    expect(shouldScheduleSignalReconnect({ ...openBase, manualDisconnect: true, timerScheduled: false })).toBe(false);
  });
});

describe("hostWaitRefreshDelayMs", () => {
  it("rejects invalid lifetimes", () => {
    expect(hostWaitRefreshDelayMs(undefined)).toBeNull();
    expect(hostWaitRefreshDelayMs("not-a-number")).toBeNull();
    expect(hostWaitRefreshDelayMs(0)).toBeNull();
    expect(hostWaitRefreshDelayMs(-1)).toBeNull();
  });

  it("refreshes before expiry and enforces a minimum delay", () => {
    expect(hostWaitRefreshDelayMs(3_000)).toBe(1_000);
    expect(hostWaitRefreshDelayMs(30_000)).toBe(25_000);
  });

  it("caps excessively long wait advertisements", () => {
    expect(hostWaitRefreshDelayMs(60 * 60 * 1_000)).toBe(HOST_WAIT_REFRESH_MAX_MS - 5_000);
  });
});

describe("shouldScheduleIceRestart", () => {
  const base = {
    manualDisconnect: false,
    currentPeer: true,
    restartInFlight: false,
    timerScheduled: false,
  } as const;

  it("allows restart for disconnected and failed peers", () => {
    expect(shouldScheduleIceRestart({ ...base, connectionState: "disconnected" })).toBe(true);
    expect(shouldScheduleIceRestart({ ...base, connectionState: "failed" })).toBe(true);
    expect(shouldScheduleIceRestart({ ...base, connectionState: "connecting" })).toBe(true);
  });

  it("blocks restart for connected or closed peers", () => {
    expect(shouldScheduleIceRestart({ ...base, connectionState: "connected" })).toBe(false);
    expect(shouldScheduleIceRestart({ ...base, connectionState: "closed" })).toBe(false);
  });

  it("blocks stale, duplicate, in-flight, and manual restart scheduling", () => {
    expect(shouldScheduleIceRestart({ ...base, currentPeer: false, connectionState: "failed" })).toBe(false);
    expect(shouldScheduleIceRestart({ ...base, restartInFlight: true, connectionState: "failed" })).toBe(false);
    expect(shouldScheduleIceRestart({ ...base, timerScheduled: true, connectionState: "failed" })).toBe(false);
    expect(shouldScheduleIceRestart({ ...base, manualDisconnect: true, connectionState: "failed" })).toBe(false);
  });
});

describe("iceRestartDelayMs", () => {
  it("distinguishes immediate failure recovery from delayed disconnect recovery", () => {
    expect(iceRestartDelayMs(true)).toBe(0);
    expect(iceRestartDelayMs(false)).toBe(2_000);
  });
});
