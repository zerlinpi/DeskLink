export type UploadRecoveryState = {
  state: string;
  streamToken: number;
};

export type DownloadRecoveryState = {
  state: string;
  token: number;
  received: number;
  requested: number;
  outstanding: number;
};

const TERMINAL_STATES = new Set(["complete", "cancelled", "error"]);

export function pauseUploadForChannelReplacement(job: UploadRecoveryState | null): boolean {
  if (!job || TERMINAL_STATES.has(job.state)) return false;
  job.streamToken += 1;
  job.state = "paused";
  return true;
}

export function pauseDownloadForChannelReplacement(job: DownloadRecoveryState | null): boolean {
  if (!job || TERMINAL_STATES.has(job.state)) return false;
  job.token += 1;
  job.state = "paused";
  job.requested = job.received;
  job.outstanding = 0;
  return true;
}
