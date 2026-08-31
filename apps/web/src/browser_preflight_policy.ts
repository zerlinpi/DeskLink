export type PreflightInteractionContext = {
  blockingCount: number;
  insideConnectCard: boolean;
  insideRecentDevices: boolean;
};

export function shouldBlockConnectionInteraction(
  context: PreflightInteractionContext,
): boolean {
  if (context.blockingCount <= 0) return false;
  if (!context.insideConnectCard) return false;
  if (context.insideRecentDevices) return false;
  return true;
}

export function isConnectAction(
  dataAction: string | undefined,
  text: string | null | undefined,
): boolean {
  if (dataAction) return dataAction === "connect";
  const normalized = text?.trim();
  return normalized === "Connect" || normalized === "连接设备";
}

export function shouldBlockEnterConnect(
  context: PreflightInteractionContext,
  hasPrimaryConnectButton: boolean,
): boolean {
  return hasPrimaryConnectButton && shouldBlockConnectionInteraction(context);
}
