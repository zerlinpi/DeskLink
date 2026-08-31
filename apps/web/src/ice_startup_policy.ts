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

export const LAN_FIRST_RELAY_FALLBACK_MS = 1_500;

export function shouldUseLanFirstIce(gate: LanFirstIceGate): boolean {
  return gate.enabled && !gate.forceRelay;
}

export function directFirstIceServers(stunUrl: string): RTCIceServer[] {
  const normalized = stunUrl.trim();
  return normalized ? [{ urls: normalized }] : [];
}

export function shouldEscalateLanFirstToRelay(gate: RelayEscalationGate): boolean {
  if (gate.manualDisconnect || !gate.currentPeer) return false;
  if (gate.restartInFlight || gate.relayEscalated) return false;
  return gate.connectionState !== "connected" && gate.connectionState !== "closed";
}
