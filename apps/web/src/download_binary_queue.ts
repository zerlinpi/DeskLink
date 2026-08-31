export const MAX_PENDING_DOWNLOAD_BINARY_FRAMES = 16;
export const DOWNLOAD_BINARY_HEADER_BYTES = 8 + 32;
export const MAX_DOWNLOAD_BINARY_PAYLOAD_BYTES = 32 * 1024;
export const MAX_DOWNLOAD_BINARY_FRAME_BYTES =
  DOWNLOAD_BINARY_HEADER_BYTES + MAX_DOWNLOAD_BINARY_PAYLOAD_BYTES;

export type DownloadBinaryQueueGate = {
  pendingFrames: number;
  frameBytes: number;
};

export function canQueueDownloadBinaryFrame(gate: DownloadBinaryQueueGate): boolean {
  if (!Number.isSafeInteger(gate.pendingFrames) || gate.pendingFrames < 0) return false;
  if (gate.pendingFrames >= MAX_PENDING_DOWNLOAD_BINARY_FRAMES) return false;
  if (!Number.isSafeInteger(gate.frameBytes)) return false;
  return gate.frameBytes > DOWNLOAD_BINARY_HEADER_BYTES &&
    gate.frameBytes <= MAX_DOWNLOAD_BINARY_FRAME_BYTES;
}
