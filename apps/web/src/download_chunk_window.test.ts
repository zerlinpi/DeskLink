import { describe, expect, it } from "vitest";
import { isRequestedDownloadChunk } from "./download_chunk_window";

const base = {
  offset: 0,
  payloadBytes: 32 * 1024,
  received: 0,
  requested: 8 * 32 * 1024,
  outstanding: 8,
  fileSize: 1024 * 1024,
};

describe("isRequestedDownloadChunk", () => {
  it("accepts a sequential chunk inside the requested receive window", () => {
    expect(isRequestedDownloadChunk(base)).toBe(true);
  });

  it("rejects zero-progress chunks", () => {
    expect(isRequestedDownloadChunk({ ...base, payloadBytes: 0 })).toBe(false);
  });

  it("rejects unsolicited chunks when no read is outstanding", () => {
    expect(isRequestedDownloadChunk({ ...base, outstanding: 0 })).toBe(false);
  });

  it("rejects chunks that run past the requested window", () => {
    expect(isRequestedDownloadChunk({
      ...base,
      requested: 16 * 1024,
    })).toBe(false);
  });

  it("rejects stale or out-of-order chunks", () => {
    expect(isRequestedDownloadChunk({ ...base, offset: 32 * 1024 })).toBe(false);
    expect(isRequestedDownloadChunk({ ...base, received: 32 * 1024 })).toBe(false);
  });

  it("rejects chunks that run past the advertised file size", () => {
    expect(isRequestedDownloadChunk({
      ...base,
      fileSize: 16 * 1024,
    })).toBe(false);
  });

  it("fails closed for unsafe or inconsistent counters", () => {
    expect(isRequestedDownloadChunk({ ...base, offset: Number.MAX_SAFE_INTEGER })).toBe(false);
    expect(isRequestedDownloadChunk({ ...base, requested: -1 })).toBe(false);
    expect(isRequestedDownloadChunk({ ...base, outstanding: Number.NaN })).toBe(false);
  });
});
