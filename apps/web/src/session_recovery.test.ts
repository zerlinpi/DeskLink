import { describe, expect, it } from "vitest";
import {
  HOST_WAIT_REFRESH_MAX_MS,
  hostWaitRefreshDelayMs,
  iceRestartDelayMs,
  shouldScheduleIceRestart,
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
