import { describe, expect, it } from "vitest";
import {
  isHostScopedSignalType,
  shouldAcceptSignalMessage,
} from "./signal_scope";

const scope = {
  hostId: "windows-host-1",
  sessionId: "session-123",
};

describe("isHostScopedSignalType", () => {
  it("marks authentication and WebRTC negotiation messages as host scoped", () => {
    for (const type of ["auth-challenge", "auth-accepted", "auth-rejected", "answer", "ice"]) {
      expect(isHostScopedSignalType(type)).toBe(true);
    }
  });

  it("leaves signaling-service lifecycle messages outside the host scope gate", () => {
    expect(isHostScopedSignalType("peer-offline")).toBe(false);
    expect(isHostScopedSignalType("device-revoked")).toBe(false);
    expect(isHostScopedSignalType("error")).toBe(false);
  });
});

describe("shouldAcceptSignalMessage", () => {
  it("accepts host-scoped messages only for the active host and session", () => {
    expect(shouldAcceptSignalMessage({
      type: "answer",
      from: scope.hostId,
      session: scope.sessionId,
    }, scope)).toBe(true);

    expect(shouldAcceptSignalMessage({
      type: "answer",
      from: "old-host",
      session: scope.sessionId,
    }, scope)).toBe(false);

    expect(shouldAcceptSignalMessage({
      type: "answer",
      from: scope.hostId,
      session: "old-session",
    }, scope)).toBe(false);
  });

  it("rejects host-scoped messages that omit peer or session identity", () => {
    expect(shouldAcceptSignalMessage({ type: "ice", session: scope.sessionId }, scope)).toBe(false);
    expect(shouldAcceptSignalMessage({ type: "ice", from: scope.hostId }, scope)).toBe(false);
  });

  it("accepts peer-offline only for the active target and session", () => {
    expect(shouldAcceptSignalMessage({
      type: "peer-offline",
      target: scope.hostId,
      session: scope.sessionId,
    }, scope)).toBe(true);
    expect(shouldAcceptSignalMessage({
      type: "peer-offline",
      target: "old-target",
      session: scope.sessionId,
    }, scope)).toBe(false);
    expect(shouldAcceptSignalMessage({
      type: "peer-offline",
      target: scope.hostId,
      session: "old-session",
    }, scope)).toBe(false);
    expect(shouldAcceptSignalMessage({
      type: "peer-offline",
      target: scope.hostId,
    }, scope)).toBe(false);
    expect(shouldAcceptSignalMessage({ type: "peer-offline" }, scope)).toBe(false);
  });

  it("accepts a device revocation only when it applies to the active target", () => {
    expect(shouldAcceptSignalMessage({
      type: "device-revoked",
      target: scope.hostId,
    }, scope)).toBe(true);
    expect(shouldAcceptSignalMessage({
      type: "device-revoked",
      target: "another-device",
    }, scope)).toBe(false);
  });

  it("fails closed when a device revocation omits its target", () => {
    expect(shouldAcceptSignalMessage({ type: "device-revoked" }, scope)).toBe(false);
    expect(shouldAcceptSignalMessage({ type: "device-revoked", target: "" }, scope)).toBe(false);
  });

  it("does not change unrelated signaling-service messages", () => {
    expect(shouldAcceptSignalMessage({ type: "error" }, scope)).toBe(true);
  });
});
