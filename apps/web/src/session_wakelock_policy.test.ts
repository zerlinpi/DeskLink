import { describe, expect, it } from "vitest";
import {
  isWakeLockRequestCurrent,
  isWakeLockSessionStatusActive,
  shouldHoldWakeLock,
} from "./session_wakelock_policy";

describe("isWakeLockSessionStatusActive", () => {
  it("keeps the screen awake while connecting, connected, or recovering", () => {
    for (const status of [
      "connecting",
      "connected",
      "disconnected",
      "checking",
      "signaling",
      "authorizing host",
      "proving host access",
      "negotiating WebRTC",
      "reconnecting network",
      "control ready · signaling reconnecting",
      "host offline · waiting for host",
    ]) {
      expect(isWakeLockSessionStatusActive(status)).toBe(true);
    }
  });

  it("releases the screen wake lock outside an active session", () => {
    for (const status of ["idle", "access code rejected", "device revoked", "signal error", ""]) {
      expect(isWakeLockSessionStatusActive(status)).toBe(false);
    }
  });
});

describe("shouldHoldWakeLock", () => {
  it("requires both an active session and a visible page", () => {
    expect(shouldHoldWakeLock("control ready", "visible")).toBe(true);
    expect(shouldHoldWakeLock("control ready", "hidden")).toBe(false);
    expect(shouldHoldWakeLock("idle", "visible")).toBe(false);
  });
});

describe("isWakeLockRequestCurrent", () => {
  it("rejects a request invalidated while the browser permission request was pending", () => {
    expect(isWakeLockRequestCurrent(4, 5, "control ready", "visible")).toBe(false);
  });

  it("rejects a request when the session or page stopped being eligible", () => {
    expect(isWakeLockRequestCurrent(5, 5, "idle", "visible")).toBe(false);
    expect(isWakeLockRequestCurrent(5, 5, "control ready", "hidden")).toBe(false);
  });

  it("accepts only the latest request for the still-active visible session", () => {
    expect(isWakeLockRequestCurrent(5, 5, "control ready", "visible")).toBe(true);
  });
});
