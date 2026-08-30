export type SignalScopeEnvelope = {
  type: string;
  from?: string;
  target?: string;
  session?: string;
};

export type ActiveSignalScope = {
  hostId: string;
  sessionId: string;
};

const HOST_SCOPED_SIGNAL_TYPES = new Set([
  "auth-challenge",
  "auth-accepted",
  "auth-rejected",
  "answer",
  "ice",
]);

export function isHostScopedSignalType(type: string): boolean {
  return HOST_SCOPED_SIGNAL_TYPES.has(type);
}

export function shouldAcceptSignalMessage(
  message: SignalScopeEnvelope,
  scope: ActiveSignalScope,
): boolean {
  if (isHostScopedSignalType(message.type)) {
    return message.from === scope.hostId && message.session === scope.sessionId;
  }

  if (message.type === "peer-offline") {
    return message.target === scope.hostId && message.session === scope.sessionId;
  }

  if (message.type === "device-revoked") {
    const revokedTarget = typeof message.target === "string" ? message.target : "";
    return !revokedTarget || revokedTarget === scope.hostId;
  }

  return true;
}
