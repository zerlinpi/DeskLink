import { describe, expect, it } from "vitest";
import {
  DOWNLOAD_BINARY_HEADER_BYTES,
  MAX_DOWNLOAD_BINARY_FRAME_BYTES,
  MAX_PENDING_DOWNLOAD_BINARY_FRAMES,
  canQueueDownloadBinaryFrame,
} from "./download_binary_queue";

describe("canQueueDownloadBinaryFrame", () => {
  it("accepts valid frames while queue capacity remains", () => {
    expect(canQueueDownloadBinaryFrame({
      pendingFrames: 0,
      frameBytes: DOWNLOAD_BINARY_HEADER_BYTES + 1,
    })).toBe(true);
    expect(canQueueDownloadBinaryFrame({
      pendingFrames: MAX_PENDING_DOWNLOAD_BINARY_FRAMES - 1,
      frameBytes: MAX_DOWNLOAD_BINARY_FRAME_BYTES,
    })).toBe(true);
  });

  it("rejects frames when the bounded queue is full", () => {
    expect(canQueueDownloadBinaryFrame({
      pendingFrames: MAX_PENDING_DOWNLOAD_BINARY_FRAMES,
      frameBytes: DOWNLOAD_BINARY_HEADER_BYTES + 1,
    })).toBe(false);
  });

  it("rejects zero-progress and oversized protocol frames before enqueue", () => {
    expect(canQueueDownloadBinaryFrame({
      pendingFrames: 0,
      frameBytes: DOWNLOAD_BINARY_HEADER_BYTES,
    })).toBe(false);
    expect(canQueueDownloadBinaryFrame({
      pendingFrames: 0,
      frameBytes: MAX_DOWNLOAD_BINARY_FRAME_BYTES + 1,
    })).toBe(false);
  });

  it("fails closed for invalid counters", () => {
    expect(canQueueDownloadBinaryFrame({
      pendingFrames: -1,
      frameBytes: DOWNLOAD_BINARY_HEADER_BYTES + 1,
    })).toBe(false);
    expect(canQueueDownloadBinaryFrame({
      pendingFrames: Number.NaN,
      frameBytes: DOWNLOAD_BINARY_HEADER_BYTES + 1,
    })).toBe(false);
    expect(canQueueDownloadBinaryFrame({
      pendingFrames: 0,
      frameBytes: Number.POSITIVE_INFINITY,
    })).toBe(false);
  });
});
