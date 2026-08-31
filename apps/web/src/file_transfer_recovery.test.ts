import { describe, expect, it } from "vitest";
import {
  isDownloadChunkCommitCurrent,
  pauseDownloadForChannelReplacement,
  pauseUploadForChannelReplacement,
} from "./file_transfer_recovery";

describe("pauseUploadForChannelReplacement", () => {
  it("invalidates the old sender loop and pauses an active upload", () => {
    const job = { state: "sending", streamToken: 7 };
    expect(pauseUploadForChannelReplacement(job)).toBe(true);
    expect(job).toEqual({ state: "paused", streamToken: 8 });
  });

  it("leaves terminal uploads untouched", () => {
    const job = { state: "complete", streamToken: 7 };
    expect(pauseUploadForChannelReplacement(job)).toBe(false);
    expect(job).toEqual({ state: "complete", streamToken: 7 });
  });
});

describe("pauseDownloadForChannelReplacement", () => {
  it("invalidates queued binary work and resumes from the confirmed received offset", () => {
    const job = {
      state: "receiving",
      token: 11,
      received: 96 * 1024,
      requested: 160 * 1024,
      outstanding: 2,
    };

    expect(pauseDownloadForChannelReplacement(job)).toBe(true);
    expect(job).toEqual({
      state: "paused",
      token: 12,
      received: 96 * 1024,
      requested: 96 * 1024,
      outstanding: 0,
    });
  });

  it("leaves terminal downloads untouched", () => {
    const job = {
      state: "cancelled",
      token: 4,
      received: 1024,
      requested: 1024,
      outstanding: 0,
    };

    expect(pauseDownloadForChannelReplacement(job)).toBe(false);
    expect(job.token).toBe(4);
    expect(job.state).toBe("cancelled");
  });
});

describe("isDownloadChunkCommitCurrent", () => {
  it("allows a prepared chunk to commit only to the active receiving job", () => {
    const job = { state: "receiving", token: 9 };
    expect(isDownloadChunkCommitCurrent(job, job, 9)).toBe(true);
  });

  it("rejects work prepared before a recovery token change", () => {
    const job = { state: "receiving", token: 10 };
    expect(isDownloadChunkCommitCurrent(job, job, 9)).toBe(false);
  });

  it("rejects work for a replaced download job", () => {
    const oldJob = { state: "receiving", token: 3 };
    const currentJob = { state: "receiving", token: 3 };
    expect(isDownloadChunkCommitCurrent(currentJob, oldJob, 3)).toBe(false);
  });

  it("rejects a job that was paused while the chunk was being prepared", () => {
    const job = { state: "paused", token: 4 };
    expect(isDownloadChunkCommitCurrent(job, job, 4)).toBe(false);
  });
});
