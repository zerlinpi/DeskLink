export type DownloadChunkWindow = {
  offset: number;
  payloadBytes: number;
  received: number;
  requested: number;
  outstanding: number;
  fileSize: number;
};

export function isRequestedDownloadChunk(window: DownloadChunkWindow): boolean {
  const {
    offset,
    payloadBytes,
    received,
    requested,
    outstanding,
    fileSize,
  } = window;

  if (!Number.isSafeInteger(offset) || offset < 0) return false;
  if (!Number.isSafeInteger(payloadBytes) || payloadBytes <= 0) return false;
  if (!Number.isSafeInteger(received) || received < 0) return false;
  if (!Number.isSafeInteger(requested) || requested < received) return false;
  if (!Number.isSafeInteger(outstanding) || outstanding <= 0) return false;
  if (!Number.isSafeInteger(fileSize) || fileSize < 0) return false;
  if (offset !== received) return false;

  const end = offset + payloadBytes;
  if (!Number.isSafeInteger(end)) return false;
  return end <= requested && end <= fileSize;
}
