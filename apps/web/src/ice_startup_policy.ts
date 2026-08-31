export type LanFirstIceGate = {
  enabled: boolean;
  forceRelay: boolean;
};

export type RelayEscalationGate = {
  manualDisconnect: boolean;
  currentPeer: boolean;
  connectionState: RTCPeerConnectionState;
  restartInFlight: boolean;
  relayEscalated: boolean;
};

export type RelayEscalationCommitGate = Omit<RelayEscalationGate, "restartInFlight">;

export const LAN_FIRST_RELAY_FALLBACK_MS = 1_500;

export function shouldUseLanFirstIce(gate: LanFirstIceGate): boolean {
  return gate.enabled && !gate.forceRelay;
}

export function directFirstIceServers(stunUrl: string): RTCIceServer[] {
  const normalized = stunUrl.trim();
  return normalized ? [{ urls: normalized }] : [];
}

export function shouldEscalateLanFirstToRelay(gate: RelayEscalationGate): boolean {
  if (gate.restartInFlight) return false;
  return shouldCommitLanFirstRelayEscalation(gate);
}

export function shouldCommitLanFirstRelayEscalation(gate: RelayEscalationCommitGate): boolean {
  if (gate.manualDisconnect || !gate.currentPeer || gate.relayEscalated) return false;
  return gate.connectionState !== "connected" && gate.connectionState !== "closed";
}
