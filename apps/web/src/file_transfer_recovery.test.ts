import { describe, expect, it } from "vitest";
import {
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
