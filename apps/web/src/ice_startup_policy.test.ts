import { describe, expect, it } from "vitest";
import {
  LAN_FIRST_RELAY_FALLBACK_MS,
  directFirstIceServers,
  shouldCommitLanFirstRelayEscalation,
  shouldEscalateLanFirstToRelay,
  shouldUseLanFirstIce,
} from "./ice_startup_policy";

describe("shouldUseLanFirstIce", () => {
  it("enables direct-first startup unless relay-only mode is forced", () => {
    expect(shouldUseLanFirstIce({ enabled: true, forceRelay: false })).toBe(true);
    expect(shouldUseLanFirstIce({ enabled: true, forceRelay: true })).toBe(false);
    expect(shouldUseLanFirstIce({ enabled: false, forceRelay: false })).toBe(false);
  });
});

describe("directFirstIceServers", () => {
  it("starts with STUN only so browser host candidates can race immediately", () => {
    expect(directFirstIceServers("stun:turn.example.com:3478")).toEqual([
      { urls: "stun:turn.example.com:3478" },
    ]);
  });

  it("allows host-candidate-only startup when STUN is intentionally disabled", () => {
    expect(directFirstIceServers("   ")).toEqual([]);
  });
});

describe("shouldEscalateLanFirstToRelay", () => {
  const base = {
    manualDisconnect: false,
    currentPeer: true,
    connectionState: "connecting" as RTCPeerConnectionState,
    restartInFlight: false,
    relayEscalated: false,
  };

  it("allows relay escalation while direct connectivity is still unresolved", () => {
    expect(shouldEscalateLanFirstToRelay(base)).toBe(true);
    expect(shouldEscalateLanFirstToRelay({ ...base, connectionState: "new" })).toBe(true);
    expect(shouldEscalateLanFirstToRelay({ ...base, connectionState: "disconnected" })).toBe(true);
    expect(shouldEscalateLanFirstToRelay({ ...base, connectionState: "failed" })).toBe(true);
  });

  it("blocks stale, duplicate, completed, and manually disconnected escalations", () => {
    expect(shouldEscalateLanFirstToRelay({ ...base, manualDisconnect: true })).toBe(false);
    expect(shouldEscalateLanFirstToRelay({ ...base, currentPeer: false })).toBe(false);
    expect(shouldEscalateLanFirstToRelay({ ...base, restartInFlight: true })).toBe(false);
    expect(shouldEscalateLanFirstToRelay({ ...base, relayEscalated: true })).toBe(false);
    expect(shouldEscalateLanFirstToRelay({ ...base, connectionState: "connected" })).toBe(false);
    expect(shouldEscalateLanFirstToRelay({ ...base, connectionState: "closed" })).toBe(false);
  });

  it("keeps the fallback budget intentionally short for LAN-first startup", () => {
    expect(LAN_FIRST_RELAY_FALLBACK_MS).toBe(1_500);
  });
});

describe("shouldCommitLanFirstRelayEscalation", () => {
  const base = {
    manualDisconnect: false,
    currentPeer: true,
    connectionState: "connecting" as RTCPeerConnectionState,
    relayEscalated: false,
  };

  it("allows a still-current unresolved relay escalation to commit", () => {
    expect(shouldCommitLanFirstRelayEscalation(base)).toBe(true);
    expect(shouldCommitLanFirstRelayEscalation({ ...base, connectionState: "failed" })).toBe(true);
  });

  it("abandons a late TURN result when direct P2P connected while credentials were loading", () => {
    expect(shouldCommitLanFirstRelayEscalation({
      ...base,
      connectionState: "connected",
    })).toBe(false);
  });

  it("abandons stale, closed, disconnected-by-user, or already committed results", () => {
    expect(shouldCommitLanFirstRelayEscalation({ ...base, currentPeer: false })).toBe(false);
    expect(shouldCommitLanFirstRelayEscalation({ ...base, manualDisconnect: true })).toBe(false);
    expect(shouldCommitLanFirstRelayEscalation({ ...base, relayEscalated: true })).toBe(false);
    expect(shouldCommitLanFirstRelayEscalation({ ...base, connectionState: "closed" })).toBe(false);
  });
});
