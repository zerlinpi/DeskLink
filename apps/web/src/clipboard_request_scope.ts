export type ClipboardPendingDirection = "local-to-remote" | "remote-to-local";

export function resetClipboardRequestsForChannelChange(
  pending: Map<string, ClipboardPendingDirection>,
  currentChannel: object | null,
  nextChannel: object,
): boolean {
  if (!currentChannel || currentChannel === nextChannel) return false;
  const hadPending = pending.size > 0;
  pending.clear();
  return hadPending;
}
