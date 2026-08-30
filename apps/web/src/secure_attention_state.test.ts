import { describe, expect, it } from "vitest";

import type { HostCapabilitiesV1 } from "./host_capabilities";
import {
  SECURE_ATTENTION_TIMEOUT_MS,
  SecureAttentionStateMachine,
} from "./secure_attention_state";

function capabilities(
  available: boolean,
  reason = "",
): HostCapabilitiesV1 {
  return {
    version: 1,
    secureAttention: {
      available,
      ...(reason ? { reason } : {}),
    },
    clipboard: { available: true },
    fileTransfer: { available: true },
    systemAudio: { available: false },
    microphone: { available: false },
    protectedDesktop: { available: false },
    multiMonitor: { available: true },
    highRefresh: { available: true },
    virtualDisplay: { available: false },
    privacyMode: { available: false },
    virtualHid: { available: false },
    gamepad: { available: false },
    codecs: [],
    maximumFps: 144,
    maximumResolution: { width: 3840, height: 2160 },
  };
}

function readyMachine() {
  const machine = new SecureAttentionStateMachine();
  machine.setContext({
    sessionActive: true,
    controlChannelOpen: true,
    capabilities: capabilities(true),
  });
  return machine;
}

describe("SecureAttentionStateMachine", () => {
  it("gates Ctrl+Alt+Del until host capability and reliable control channel are ready", () => {
    const machine = new SecureAttentionStateMachine();
    expect(machine.view()).toMatchObject({
      phase: "waiting-capabilities",
      enabled: false,
    });

    machine.setContext({ capabilities: capabilities(true) });
    expect(machine.view()).toMatchObject({
      phase: "ready",
      enabled: false,
      reason: "session-or-channel-unavailable",
    });

    machine.setContext({ sessionActive: true, controlChannelOpen: true });
    expect(machine.view()).toMatchObject({
      phase: "ready",
      enabled: true,
    });
  });

  it("returns policy-not-allowed as a disabled capability instead of sending a request", () => {
    const machine = new SecureAttentionStateMachine();
    machine.setContext({
      sessionActive: true,
      controlChannelOpen: true,
      capabilities: capabilities(false, "policy-not-allowed"),
    });

    expect(machine.view()).toMatchObject({
      phase: "unavailable",
      enabled: false,
      label: "策略未允许",
      reason: "policy-not-allowed",
    });
    expect(machine.request("sas-12345678", 1000)).toBeNull();
  });

  it("returns API unavailable as a disabled capability", () => {
    const machine = new SecureAttentionStateMachine();
    machine.setContext({
      sessionActive: true,
      controlChannelOpen: true,
      capabilities: capabilities(false, "api-unavailable"),
    });

    expect(machine.view()).toMatchObject({
      phase: "unavailable",
      label: "系统不支持",
      reason: "api-unavailable",
    });
  });

  it("binds one requestId and ignores mismatched operation results", () => {
    const machine = readyMachine();
    const request = machine.request("sas-a1b2c3d4", 10_000);

    expect(request).toEqual({
      t: "system-operation",
      operation: "secure-attention-sequence",
      requestId: "sas-a1b2c3d4",
    });
    expect(machine.view()).toMatchObject({
      phase: "pending",
      enabled: false,
      requestId: "sas-a1b2c3d4",
    });
    expect(machine.request("sas-second12", 10_001)).toBeNull();

    expect(machine.handleResult({
      t: "system-operation-result",
      operation: "secure-attention-sequence",
      requestId: "sas-wrong000",
      ok: true,
    })).toBe(false);
    expect(machine.view().phase).toBe("pending");
  });

  it("times out a pending request and does not accept its late result", () => {
    const machine = readyMachine();
    machine.request("sas-timeout01", 2_000);

    expect(machine.expire(2_000 + SECURE_ATTENTION_TIMEOUT_MS - 1)).toBe(false);
    expect(machine.expire(2_000 + SECURE_ATTENTION_TIMEOUT_MS)).toBe(true);
    expect(machine.view()).toMatchObject({
      phase: "error",
      label: "请求超时",
      reason: "timeout",
    });

    expect(machine.handleResult({
      t: "system-operation-result",
      operation: "secure-attention-sequence",
      requestId: "sas-timeout01",
      ok: true,
    })).toBe(false);
  });

  it("maps rate-limited result to a stable user-visible state", () => {
    const machine = readyMachine();
    machine.request("sas-rate0001", 1);
    expect(machine.handleResult({
      t: "system-operation-result",
      operation: "secure-attention-sequence",
      requestId: "sas-rate0001",
      ok: false,
      errorCode: "rate-limited",
      error: "secure attention request rate limited",
    })).toBe(true);

    expect(machine.view()).toMatchObject({
      phase: "error",
      label: "操作过快",
      reason: "rate-limited",
    });
  });

  it("maps Service policy failure from the structured error code", () => {
    const machine = readyMachine();
    machine.request("sas-policy01", 1);
    machine.handleResult({
      t: "system-operation-result",
      operation: "secure-attention-sequence",
      requestId: "sas-policy01",
      ok: false,
      errorCode: "policy-not-allowed",
    });

    expect(machine.view()).toMatchObject({
      phase: "error",
      label: "策略未允许",
      reason: "policy-not-allowed",
    });
  });

  it("cancels pending authority immediately when the control channel closes", () => {
    const machine = readyMachine();
    machine.request("sas-close0001", 1);
    machine.setContext({ controlChannelOpen: false });

    expect(machine.view()).toMatchObject({
      phase: "ready",
      enabled: false,
      reason: "session-or-channel-unavailable",
      requestId: "",
    });
  });

  it("can recover after reconnect without retaining the previous request", () => {
    const machine = readyMachine();
    machine.request("sas-before001", 1);
    machine.reset();

    expect(machine.view().phase).toBe("waiting-capabilities");
    machine.setContext({
      sessionActive: true,
      controlChannelOpen: true,
      capabilities: capabilities(true),
    });
    expect(machine.view()).toMatchObject({ phase: "ready", enabled: true });
    expect(machine.request("sas-after0001", 100)).not.toBeNull();
  });

  it("rejects malformed request identifiers fail-closed", () => {
    const machine = readyMachine();
    expect(machine.request("", 1)).toBeNull();
    expect(machine.request("bad request id", 1)).toBeNull();
    expect(machine.request("x".repeat(129), 1)).toBeNull();
    expect(machine.view()).toMatchObject({ phase: "ready", enabled: true });
  });
});
