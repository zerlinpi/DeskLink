import {
  pauseDownloadForChannelReplacement,
  pauseUploadForChannelReplacement,
} from "./file_transfer_recovery";

type ControlChannelDetail = {
  channel: RTCDataChannel;
  peer: RTCPeerConnection;
};

type UploadState =
  | "queued"
  | "waiting"
  | "sending"
  | "verifying"
  | "paused"
  | "cancelling"
  | "complete"
  | "cancelled"
  | "error";

type UploadJob = {
  id: string;
  file: File;
  state: UploadState;
  offset: number;
  confirmed: number;
  error: string;
  cancelRequested: boolean;
  streamToken: number;
};

type UploadMessage = Record<string, unknown> & {
  t?: unknown;
  id?: unknown;
};

type RemoteFile = {
  name: string;
  size: number;
};

type FileSystemWritableLike = {
  write(data: BufferSource | Blob | string | {
    type: "write";
    position: number;
    data: BufferSource | Blob | string;
  }): Promise<void>;
  close(): Promise<void>;
  abort?(reason?: unknown): Promise<void>;
};

type FileSystemHandleLike = {
  createWritable(options?: { keepExistingData?: boolean }): Promise<FileSystemWritableLike>;
};

type FilePickerWindow = Window & typeof globalThis & {
  showSaveFilePicker?: (options?: { suggestedName?: string }) => Promise<FileSystemHandleLike>;
};

type DownloadSink =
  | { kind: "filesystem"; writable: FileSystemWritableLike }
  | { kind: "memory"; chunks: BlobPart[] };

type DownloadState = "waiting" | "receiving" | "paused" | "complete" | "cancelled" | "error";

type DownloadJob = {
  id: string;
  file: RemoteFile;
  sink: DownloadSink;
  state: DownloadState;
  received: number;
  requested: number;
  outstanding: number;
  token: number;
  error: string;
};

const CHUNK_BYTES = 32 * 1024;
const CHUNK_HEADER_BYTES = 8 + 32;
const BUFFER_LOW_BYTES = 512 * 1024;
const BUFFER_HIGH_BYTES = 2 * 1024 * 1024;
const DOWNLOAD_WINDOW_CHUNKS = 8;
const MAX_TRANSFER_BYTES = 20 * 1024 * 1024 * 1024;
const MAX_MEMORY_DOWNLOAD_BYTES = 256 * 1024 * 1024;

let peer: RTCPeerConnection | null = null;
let transferChannel: RTCDataChannel | null = null;
let activeJob: UploadJob | null = null;
const jobs: UploadJob[] = [];
let remoteFiles: RemoteFile[] = [];
let activeDownload: DownloadJob | null = null;
let downloadProcessChain: Promise<void> = Promise.resolve();

let transferButton: HTMLButtonElement | null = null;
let transferPanel: HTMLDivElement | null = null;
let transferList: HTMLDivElement | null = null;
let remoteFileList: HTMLDivElement | null = null;
let downloadProgress: HTMLDivElement | null = null;
let transferStatus: HTMLSpanElement | null = null;
let fileInput: HTMLInputElement | null = null;
let dropOverlay: HTMLDivElement | null = null;
let dragDepth = 0;

function transferId() {
  return crypto.randomUUID().replaceAll("-", "");
}

function formatBytes(bytes: number) {
  if (!Number.isFinite(bytes) || bytes <= 0) return "0 B";
  const units = ["B", "KiB", "MiB", "GiB", "TiB"];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  const digits = unit === 0 ? 0 : value >= 100 ? 0 : value >= 10 ? 1 : 2;
  return `${value.toFixed(digits)} ${units[unit]}`;
}

function stateLabel(job: UploadJob) {
  switch (job.state) {
    case "queued": return "等待发送";
    case "waiting": return "协商续传位置…";
    case "sending": return `发送中 · ${Math.floor((job.offset / Math.max(1, job.file.size)) * 100)}%`;
    case "verifying": return "远端落盘中…";
    case "paused": return "网络中断 · 等待自动续传";
    case "cancelling": return "正在取消…";
    case "complete": return "已发送到远端";
    case "cancelled": return "已取消";
    case "error": return job.error || "传输失败";
  }
}

function downloadStateLabel(job: DownloadJob) {
  switch (job.state) {
    case "waiting": return "准备下载…";
    case "receiving": return `下载中 · ${Math.floor((job.received / Math.max(1, job.file.size)) * 100)}%`;
    case "paused": return "网络中断 · 等待自动续传";
    case "complete": return "下载完成";
    case "cancelled": return "已取消";
    case "error": return job.error || "下载失败";
  }
}

function setStatus(value: string, error = false) {
  if (!transferStatus) return;
  transferStatus.textContent = value;
  transferStatus.classList.toggle("is-error", error);
}

function sessionActive() {
  return document.body.classList.contains("remote-session-active");
}

function channelReady() {
  return transferChannel?.readyState === "open";
}

function sendText(message: Record<string, unknown>) {
  const channel = transferChannel;
  if (!channel || channel.readyState !== "open") return false;
  try {
    channel.send(JSON.stringify(message));
    return true;
  } catch (error) {
    console.debug("DeskLink file transfer control send failed", error);
    return false;
  }
}

function renderJobs() {
  if (transferButton) {
    const pendingUploads = jobs.filter((job) => !["complete", "cancelled", "error"].includes(job.state)).length;
    const pendingDownloads = activeDownload && !["complete", "cancelled", "error"].includes(activeDownload.state) ? 1 : 0;
    const pending = pendingUploads + pendingDownloads;
    transferButton.textContent = pending > 0 ? `文件 · ${pending}` : "文件";
    transferButton.classList.toggle("is-active", pending > 0);
  }
  if (!transferList) return;
  transferList.replaceChildren();

  if (jobs.length === 0) {
    const empty = document.createElement("div");
    empty.className = "transfer-empty";
    empty.textContent = "把文件拖到远程画面，或点击“选择文件”。";
    transferList.append(empty);
    return;
  }

  for (const job of jobs) {
    const row = document.createElement("div");
    row.className = "transfer-item";
    row.dataset.state = job.state;

    const header = document.createElement("div");
    header.className = "transfer-item-header";
    const name = document.createElement("strong");
    name.textContent = job.file.name;
    name.title = job.file.name;
    const size = document.createElement("span");
    size.textContent = formatBytes(job.file.size);
    header.append(name, size);

    const progress = document.createElement("div");
    progress.className = "transfer-progress";
    const bar = document.createElement("span");
    const ratio = job.file.size === 0
      ? (job.state === "complete" ? 1 : 0)
      : Math.max(0, Math.min(1, job.offset / job.file.size));
    bar.style.width = `${(ratio * 100).toFixed(2)}%`;
    progress.append(bar);

    const footer = document.createElement("div");
    footer.className = "transfer-item-footer";
    const state = document.createElement("span");
    state.textContent = stateLabel(job);
    state.classList.toggle("is-error", job.state === "error");
    footer.append(state);

    if (job.state === "error") {
      const retry = document.createElement("button");
      retry.type = "button";
      retry.textContent = "续传重试";
      retry.addEventListener("click", () => retryJob(job));
      footer.append(retry);
    } else if (!["complete", "cancelled"].includes(job.state)) {
      const cancel = document.createElement("button");
      cancel.type = "button";
      cancel.textContent = "取消";
      cancel.addEventListener("click", () => cancelJob(job));
      footer.append(cancel);
    }

    row.append(header, progress, footer);
    transferList.append(row);
  }
}

function renderRemoteFiles() {
  if (!remoteFileList) return;
  remoteFileList.replaceChildren();
  if (remoteFiles.length === 0) {
    const empty = document.createElement("div");
    empty.className = "transfer-empty";
    empty.textContent = "远端 DeskLink 目录暂无可下载文件。";
    remoteFileList.append(empty);
    return;
  }

  for (const file of remoteFiles) {
    const row = document.createElement("div");
    row.className = "remote-file-item";
    const info = document.createElement("div");
    const name = document.createElement("strong");
    name.textContent = file.name;
    name.title = file.name;
    const size = document.createElement("span");
    size.textContent = formatBytes(file.size);
    info.append(name, size);

    const download = document.createElement("button");
    download.type = "button";
    download.textContent = "下载";
    download.disabled = Boolean(activeDownload && !["complete", "cancelled", "error"].includes(activeDownload.state));
    download.addEventListener("click", () => void startDownload(file));
    row.append(info, download);
    remoteFileList.append(row);
  }
}

function renderDownload() {
  if (!downloadProgress) return;
  downloadProgress.replaceChildren();
  const job = activeDownload;
  if (!job) {
    downloadProgress.hidden = true;
    renderJobs();
    renderRemoteFiles();
    return;
  }

  downloadProgress.hidden = false;
  const row = document.createElement("div");
  row.className = "transfer-item download-item";
  row.dataset.state = job.state;

  const header = document.createElement("div");
  header.className = "transfer-item-header";
  const name = document.createElement("strong");
  name.textContent = `↓ ${job.file.name}`;
  name.title = job.file.name;
  const size = document.createElement("span");
  size.textContent = formatBytes(job.file.size);
  header.append(name, size);

  const progress = document.createElement("div");
  progress.className = "transfer-progress";
  const bar = document.createElement("span");
  const ratio = job.file.size === 0
    ? (job.state === "complete" ? 1 : 0)
    : Math.max(0, Math.min(1, job.received / job.file.size));
  bar.style.width = `${(ratio * 100).toFixed(2)}%`;
  progress.append(bar);

  const footer = document.createElement("div");
  footer.className = "transfer-item-footer";
  const state = document.createElement("span");
  state.textContent = downloadStateLabel(job);
  state.classList.toggle("is-error", job.state === "error");
  footer.append(state);
  if (!["complete", "cancelled", "error"].includes(job.state)) {
    const cancel = document.createElement("button");
    cancel.type = "button";
    cancel.textContent = "取消";
    cancel.addEventListener("click", () => void cancelDownload());
    footer.append(cancel);
  }

  row.append(header, progress, footer);
  downloadProgress.append(row);
  renderJobs();
  renderRemoteFiles();
}

function closePanel() {
  if (!transferPanel) return;
  transferPanel.hidden = true;
  transferButton?.setAttribute("aria-expanded", "false");
}

function beginJob(job: UploadJob) {
  if (!channelReady()) {
    job.state = "paused";
    renderJobs();
    return false;
  }
  job.streamToken += 1;
  job.state = job.cancelRequested ? "cancelling" : "waiting";
  job.error = "";
  if (!sendText({
    t: "upload-begin",
    id: job.id,
    name: job.file.name,
    size: job.file.size,
  })) {
    job.state = "paused";
    renderJobs();
    return false;
  }
  setStatus(job.cancelRequested ? "正在恢复连接以取消远端临时文件…" : `准备发送 ${job.file.name}`);
  renderJobs();
  return true;
}

function pumpQueue() {
  if (!channelReady()) return;
  if (activeJob && !["complete", "cancelled", "error"].includes(activeJob.state)) {
    if (activeJob.state === "paused") beginJob(activeJob);
    return;
  }

  activeJob = jobs.find((job) => job.state === "queued") ?? null;
  if (activeJob) beginJob(activeJob);
}

function waitForCapacity(channel: RTCDataChannel, job: UploadJob, token: number) {
  if (channel.bufferedAmount <= BUFFER_HIGH_BYTES) return Promise.resolve(true);

  return new Promise<boolean>((resolve) => {
    let settled = false;
    const finish = (value: boolean) => {
      if (settled) return;
      settled = true;
      channel.removeEventListener("bufferedamountlow", onLow);
      channel.removeEventListener("close", onClose);
      resolve(value);
    };
    const onLow = () => finish(
      transferChannel === channel &&
      channel.readyState === "open" &&
      activeJob === job &&
      job.streamToken === token,
    );
    const onClose = () => finish(false);

    channel.bufferedAmountLowThreshold = BUFFER_LOW_BYTES;
    channel.addEventListener("bufferedamountlow", onLow, { once: true });
    channel.addEventListener("close", onClose, { once: true });

    if (channel.bufferedAmount <= BUFFER_LOW_BYTES) onLow();
  });
}

async function streamJob(job: UploadJob, channel: RTCDataChannel, token: number) {
  try {
    while (job.offset < job.file.size) {
      if (transferChannel !== channel || channel.readyState !== "open" ||
          activeJob !== job || job.streamToken !== token || job.cancelRequested) {
        return;
      }

      if (!await waitForCapacity(channel, job, token)) return;
      if (transferChannel !== channel || channel.readyState !== "open" ||
          activeJob !== job || job.streamToken !== token || job.cancelRequested) {
        return;
      }

      const offset = job.offset;
      const end = Math.min(job.file.size, offset + CHUNK_BYTES);
      const payload = await job.file.slice(offset, end).arrayBuffer();
      const digest = await crypto.subtle.digest("SHA-256", payload);

      if (transferChannel !== channel || channel.readyState !== "open" ||
          activeJob !== job || job.streamToken !== token || job.cancelRequested) {
        return;
      }

      const frame = new Uint8Array(CHUNK_HEADER_BYTES + payload.byteLength);
      const view = new DataView(frame.buffer);
      view.setBigUint64(0, BigInt(offset), true);
      frame.set(new Uint8Array(digest), 8);
      frame.set(new Uint8Array(payload), CHUNK_HEADER_BYTES);
      channel.send(frame.buffer);
      job.offset = end;
      renderJobs();
    }

    if (activeJob === job && job.streamToken === token && !job.cancelRequested) {
      job.state = "verifying";
      setStatus(`已发送 ${job.file.name}，等待远端完成落盘…`);
      renderJobs();
    }
  } catch (error) {
    console.debug("DeskLink file transfer stream failed", error);
    if (activeJob !== job || job.streamToken !== token) return;
    if (!channelReady()) {
      job.state = "paused";
      setStatus("网络中断，连接恢复后将从远端确认的偏移继续", false);
    } else {
      job.state = "error";
      job.error = "浏览器读取或发送文件失败";
      setStatus(job.error, true);
      activeJob = null;
      queueMicrotask(pumpQueue);
    }
    renderJobs();
  }
}

function handleUploadMessage(message: UploadMessage) {
  const id = typeof message.id === "string" ? message.id : "";
  const job = activeJob;
  if (!job || id !== job.id) return;

  if (message.t === "upload-ready") {
    const offset = typeof message.offset === "number" ? message.offset : NaN;
    if (!Number.isSafeInteger(offset) || offset < 0 || offset > job.file.size) {
      job.state = "error";
      job.error = "远端返回了无效的续传位置";
      activeJob = null;
      setStatus(job.error, true);
      renderJobs();
      queueMicrotask(pumpQueue);
      return;
    }

    job.offset = offset;
    job.confirmed = offset;
    if (job.cancelRequested) {
      job.state = "cancelling";
      sendText({ t: "upload-cancel", id: job.id });
      renderJobs();
      return;
    }

    job.state = "sending";
    const token = ++job.streamToken;
    setStatus(offset > 0
      ? `已从 ${formatBytes(offset)} 断点续传 ${job.file.name}`
      : `正在发送 ${job.file.name}`);
    renderJobs();
    const channel = transferChannel;
    if (channel) void streamJob(job, channel, token);
    return;
  }

  if (message.t === "upload-progress") {
    const received = typeof message.received === "number" ? message.received : NaN;
    if (Number.isSafeInteger(received) && received >= job.confirmed && received <= job.file.size) {
      job.confirmed = received;
    }
    return;
  }

  if (message.t === "upload-complete") {
    job.offset = job.file.size;
    job.confirmed = job.file.size;
    job.state = "complete";
    job.error = "";
    activeJob = null;
    const savedName = typeof message.name === "string" && message.name ? message.name : job.file.name;
    setStatus(`已发送：${savedName}`);
    renderJobs();
    requestRemoteFiles();
    queueMicrotask(pumpQueue);
    return;
  }

  if (message.t === "upload-cancelled") {
    job.state = "cancelled";
    activeJob = null;
    setStatus(`已取消 ${job.file.name}`);
    renderJobs();
    queueMicrotask(pumpQueue);
    return;
  }

  if (message.t === "upload-error") {
    const reason = typeof message.error === "string" ? message.error : "remote-transfer-error";
    job.state = "error";
    job.error = reason;
    job.streamToken += 1;
    activeJob = null;
    setStatus(`文件传输失败：${reason}；可从已校验位置重试`, true);
    renderJobs();
    queueMicrotask(pumpQueue);
  }
}

function requestRemoteFiles() {
  if (!sendText({ t: "download-list-request" })) {
    setStatus("远程文件通道未连接", true);
    return;
  }
  setStatus("正在读取远端 DeskLink 文件列表…");
}

function normalizeRemoteFiles(raw: unknown): RemoteFile[] {
  if (!Array.isArray(raw)) return [];
  const output: RemoteFile[] = [];
  for (const item of raw) {
    if (!item || typeof item !== "object") continue;
    const record = item as Record<string, unknown>;
    if (typeof record.name !== "string" || typeof record.size !== "number" ||
        !Number.isSafeInteger(record.size) || record.size < 0 || record.size > MAX_TRANSFER_BYTES) {
      continue;
    }
    output.push({ name: record.name, size: record.size });
  }
  return output.slice(0, 200);
}

async function makeDownloadSink(file: RemoteFile): Promise<DownloadSink> {
  const picker = (window as FilePickerWindow).showSaveFilePicker;
  if (picker) {
    const handle = await picker({ suggestedName: file.name });
    const writable = await handle.createWritable({ keepExistingData: false });
    return { kind: "filesystem", writable };
  }
  if (file.size > MAX_MEMORY_DOWNLOAD_BYTES) {
    throw new Error("此浏览器不支持流式保存，超过 256 MiB 的远端文件请使用最新版 Chrome/Edge");
  }
  return { kind: "memory", chunks: [] };
}

function beginDownload(job: DownloadJob) {
  if (!channelReady()) {
    job.state = "paused";
    renderDownload();
    return;
  }
  job.token += 1;
  job.state = "waiting";
  job.requested = job.received;
  job.outstanding = 0;
  if (!sendText({
    t: "download-begin",
    id: job.id,
    name: job.file.name,
    offset: job.received,
  })) {
    job.state = "paused";
  }
  renderDownload();
}

async function startDownload(file: RemoteFile) {
  if (activeDownload && !["complete", "cancelled", "error"].includes(activeDownload.state)) {
    setStatus("一次只能下载一个远端文件，请先完成或取消当前下载", true);
    return;
  }

  try {
    const sink = await makeDownloadSink(file);
    activeDownload = {
      id: transferId(),
      file,
      sink,
      state: "waiting",
      received: 0,
      requested: 0,
      outstanding: 0,
      token: 0,
      error: "",
    };
    setStatus(`准备下载 ${file.name}`);
    renderDownload();
    beginDownload(activeDownload);
  } catch (error) {
    const message = error instanceof DOMException && error.name === "AbortError"
      ? "已取消选择保存位置"
      : (error instanceof Error ? error.message : "无法准备本地保存位置");
    setStatus(message, !(error instanceof DOMException && error.name === "AbortError"));
  }
}

function pumpDownloadReads(job: DownloadJob) {
  if (activeDownload !== job || job.state !== "receiving" || !channelReady()) return;
  while (job.outstanding < DOWNLOAD_WINDOW_CHUNKS && job.requested < job.file.size) {
    const length = Math.min(CHUNK_BYTES, job.file.size - job.requested);
    if (!sendText({
      t: "download-read",
      id: job.id,
      offset: job.requested,
      length,
    })) {
      job.state = "paused";
      renderDownload();
      return;
    }
    job.requested += length;
    job.outstanding += 1;
  }
}

async function writeDownloadChunk(job: DownloadJob, offset: number, payload: Uint8Array) {
  if (job.sink.kind === "filesystem") {
    const copy = new ArrayBuffer(payload.byteLength);
    new Uint8Array(copy).set(payload);
    await job.sink.writable.write({ type: "write", position: offset, data: copy });
  } else {
    job.sink.chunks.push(payload.slice());
  }
}

function digestEqual(expected: Uint8Array, actual: Uint8Array) {
  if (expected.byteLength !== actual.byteLength) return false;
  let difference = 0;
  for (let index = 0; index < expected.byteLength; index += 1) {
    difference |= expected[index] ^ actual[index];
  }
  return difference === 0;
}

async function failDownload(job: DownloadJob, message: string, cancelRemote = true) {
  if (activeDownload !== job) return;
  job.token += 1;
  job.state = "error";
  job.error = message;
  if (cancelRemote && channelReady()) sendText({ t: "download-cancel", id: job.id });
  if (job.sink.kind === "filesystem" && job.sink.writable.abort) {
    try { await job.sink.writable.abort(message); } catch { /* ignored */ }
  }
  setStatus(message, true);
  renderDownload();
}

async function finishDownload(job: DownloadJob) {
  if (activeDownload !== job || job.state === "complete") return;
  if (job.sink.kind === "filesystem") {
    await job.sink.writable.close();
  } else {
    const blob = new Blob(job.sink.chunks);
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = job.file.name;
    anchor.style.display = "none";
    document.body.append(anchor);
    anchor.click();
    anchor.remove();
    window.setTimeout(() => URL.revokeObjectURL(url), 60_000);
  }
  job.state = "complete";
  setStatus(`已下载：${job.file.name}`);
  renderDownload();
}

async function processDownloadBinary(buffer: ArrayBuffer, token: number) {
  const job = activeDownload;
  if (!job || job.token !== token || job.state !== "receiving") return;
  const bytes = new Uint8Array(buffer);
  if (bytes.byteLength < CHUNK_HEADER_BYTES || bytes.byteLength > CHUNK_HEADER_BYTES + CHUNK_BYTES) {
    await failDownload(job, "远端返回了非法文件分块");
    return;
  }

  const view = new DataView(buffer, bytes.byteOffset, bytes.byteLength);
  const rawOffset = view.getBigUint64(0, true);
  if (rawOffset > BigInt(Number.MAX_SAFE_INTEGER)) {
    await failDownload(job, "远端文件偏移超出浏览器安全范围");
    return;
  }
  const offset = Number(rawOffset);
  const expectedDigest = bytes.slice(8, 40);
  const payload = bytes.slice(CHUNK_HEADER_BYTES);
  if (offset !== job.received || offset + payload.byteLength > job.file.size) {
    await failDownload(job, "远端文件分块顺序不一致");
    return;
  }

  const actualDigest = new Uint8Array(await crypto.subtle.digest("SHA-256", payload));
  if (!digestEqual(expectedDigest, actualDigest)) {
    await failDownload(job, "远端文件分块 SHA-256 校验失败");
    return;
  }

  try {
    await writeDownloadChunk(job, offset, payload);
  } catch (error) {
    console.debug("DeskLink local download write failed", error);
    await failDownload(job, "写入本地文件失败");
    return;
  }

  if (activeDownload !== job || job.token !== token) return;
  job.received += payload.byteLength;
  job.outstanding = Math.max(0, job.outstanding - 1);
  renderDownload();
  if (job.received === job.file.size) {
    try {
      await finishDownload(job);
    } catch (error) {
      console.debug("DeskLink download finalize failed", error);
      await failDownload(job, "完成本地文件保存失败", false);
    }
    return;
  }
  pumpDownloadReads(job);
}

async function cancelDownload() {
  const job = activeDownload;
  if (!job || ["complete", "cancelled", "error"].includes(job.state)) return;
  job.token += 1;
  if (channelReady()) sendText({ t: "download-cancel", id: job.id });
  if (job.sink.kind === "filesystem" && job.sink.writable.abort) {
    try { await job.sink.writable.abort("cancelled"); } catch { /* ignored */ }
  }
  job.state = "cancelled";
  setStatus(`已取消下载 ${job.file.name}`);
  renderDownload();
}

function handleDownloadMessage(message: UploadMessage) {
  if (message.t === "download-list") {
    remoteFiles = normalizeRemoteFiles(message.files);
    renderRemoteFiles();
    const error = typeof message.error === "string" ? message.error : "";
    setStatus(error ? `读取远端文件失败：${error}` : `远端共有 ${remoteFiles.length} 个可下载文件`, Boolean(error));
    return true;
  }

  const job = activeDownload;
  const id = typeof message.id === "string" ? message.id : "";
  if (!job || id !== job.id) return false;

  if (message.t === "download-ready") {
    const offset = typeof message.offset === "number" ? message.offset : NaN;
    const size = typeof message.size === "number" ? message.size : NaN;
    if (!Number.isSafeInteger(offset) || offset !== job.received ||
        !Number.isSafeInteger(size) || size !== job.file.size) {
      void failDownload(job, "远端文件在下载过程中发生变化");
      return true;
    }
    job.state = "receiving";
    job.requested = offset;
    job.outstanding = 0;
    setStatus(offset > 0
      ? `从 ${formatBytes(offset)} 继续下载 ${job.file.name}`
      : `正在下载 ${job.file.name}`);
    renderDownload();
    if (job.file.size === 0) {
      void finishDownload(job).catch(() => failDownload(job, "完成空文件保存失败", false));
    } else {
      pumpDownloadReads(job);
    }
    return true;
  }

  if (message.t === "download-error") {
    const reason = typeof message.error === "string" ? message.error : "remote-download-error";
    void failDownload(job, `远端下载失败：${reason}`, false);
    return true;
  }

  if (message.t === "download-cancelled") return true;
  if (message.t === "download-complete") return true;
  return false;
}

function pauseTransfersForChannelReplacement() {
  const uploadPaused = pauseUploadForChannelReplacement(activeJob);
  const downloadPaused = pauseDownloadForChannelReplacement(activeDownload);
  if (!uploadPaused && !downloadPaused) return;

  setStatus("文件通道正在恢复，将从已确认位置自动续传");
  renderJobs();
  renderDownload();
}

function attachTransferChannel(channel: RTCDataChannel) {
  if (transferChannel && transferChannel !== channel) {
    pauseTransfersForChannelReplacement();
    if (transferChannel.readyState !== "closed") {
      try { transferChannel.close(); } catch { /* ignored */ }
    }
  }
  transferChannel = channel;
  channel.binaryType = "arraybuffer";
  channel.bufferedAmountLowThreshold = BUFFER_LOW_BYTES;

  channel.addEventListener("open", () => {
    if (transferChannel !== channel) return;
    setStatus("WebRTC 文件通道已就绪");
    pumpQueue();
    if (activeDownload && activeDownload.state === "paused") beginDownload(activeDownload);
    requestRemoteFiles();
  });

  channel.addEventListener("message", (event) => {
    if (transferChannel !== channel) return;
    if (typeof event.data === "string") {
      if (event.data.length > 64 * 1024) return;
      try {
        const parsed = JSON.parse(event.data) as unknown;
        if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) return;
        const message = parsed as UploadMessage;
        if (!handleDownloadMessage(message)) handleUploadMessage(message);
      } catch {
        // Ignore invalid application messages.
      }
      return;
    }

    if (event.data instanceof ArrayBuffer) {
      const job = activeDownload;
      if (!job) return;
      const token = job.token;
      const buffer = event.data;
      downloadProcessChain = downloadProcessChain
        .then(() => processDownloadBinary(buffer, token))
        .catch((error) => {
          console.debug("DeskLink download processing chain failed", error);
        });
    }
  });

  channel.addEventListener("close", () => {
    if (transferChannel !== channel) return;
    transferChannel = null;
    if (activeJob && !["complete", "cancelled", "error"].includes(activeJob.state)) {
      activeJob.streamToken += 1;
      activeJob.state = "paused";
    }
    if (activeDownload && !["complete", "cancelled", "error"].includes(activeDownload.state)) {
      activeDownload.token += 1;
      activeDownload.state = "paused";
      activeDownload.requested = activeDownload.received;
      activeDownload.outstanding = 0;
    }
    setStatus("文件通道已断开，网络恢复后自动从已确认位置续传");
    renderJobs();
    renderDownload();
  });
}

function attachPeer(nextPeer: RTCPeerConnection) {
  if (peer === nextPeer && transferChannel && transferChannel.readyState !== "closed") return;
  peer = nextPeer;
  try {
    const channel = nextPeer.createDataChannel("file-transfer", { ordered: true });
    attachTransferChannel(channel);
  } catch (error) {
    console.debug("DeskLink file transfer channel creation failed", error);
    setStatus("无法创建 WebRTC 文件通道", true);
  }
}

function enqueueFiles(fileList: FileList | File[]) {
  const files = Array.from(fileList);
  if (files.length === 0) return;

  let accepted = 0;
  for (const file of files) {
    if (!file.name || file.size > MAX_TRANSFER_BYTES) {
      setStatus(file.size > MAX_TRANSFER_BYTES
        ? `${file.name || "文件"} 超过 20 GiB 上限`
        : "无法发送无文件名项目", true);
      continue;
    }
    jobs.push({
      id: transferId(),
      file,
      state: "queued",
      offset: 0,
      confirmed: 0,
      error: "",
      cancelRequested: false,
      streamToken: 0,
    });
    accepted += 1;
  }

  if (accepted > 0) {
    setStatus(channelReady() ? `已加入 ${accepted} 个文件` : `已加入 ${accepted} 个文件，等待远程文件通道`);
    renderJobs();
    pumpQueue();
  }
}

function retryJob(job: UploadJob) {
  if (job.state !== "error") return;
  job.state = "queued";
  job.error = "";
  job.cancelRequested = false;
  job.streamToken += 1;
  job.offset = job.confirmed;
  setStatus(`准备从远端已校验位置重试 ${job.file.name}`);
  renderJobs();
  pumpQueue();
}

function cancelJob(job: UploadJob) {
  if (["complete", "cancelled", "error"].includes(job.state)) return;
  job.cancelRequested = true;
  job.streamToken += 1;

  if (activeJob !== job) {
    job.state = "cancelled";
    renderJobs();
    return;
  }

  job.state = "cancelling";
  if (channelReady()) {
    if (!sendText({ t: "upload-cancel", id: job.id })) job.state = "paused";
  } else {
    job.state = "paused";
    setStatus("连接恢复后会清理远端临时文件");
  }
  renderJobs();
}

function clearFinished() {
  for (let index = jobs.length - 1; index >= 0; index -= 1) {
    if (["complete", "cancelled", "error"].includes(jobs[index].state)) jobs.splice(index, 1);
  }
  if (activeDownload && ["complete", "cancelled", "error"].includes(activeDownload.state)) {
    activeDownload = null;
  }
  renderJobs();
  renderDownload();
}

function mountTransferControl() {
  const actions = document.querySelector<HTMLElement>(".workbench-actions");
  const stage = document.querySelector<HTMLElement>(".stage");
  if (!actions || !stage || actions.querySelector(".file-transfer-control")) return;

  const wrapper = document.createElement("div");
  wrapper.className = "file-transfer-control";

  transferButton = document.createElement("button");
  transferButton.type = "button";
  transferButton.className = "workbench-button file-transfer-trigger";
  transferButton.textContent = "文件";
  transferButton.setAttribute("aria-haspopup", "dialog");
  transferButton.setAttribute("aria-expanded", "false");
  transferButton.title = "WebRTC 双向文件传输：拖拽上传、远端文件下载、网络重连续传";

  transferPanel = document.createElement("div");
  transferPanel.className = "file-transfer-panel";
  transferPanel.hidden = true;
  transferPanel.setAttribute("role", "dialog");
  transferPanel.setAttribute("aria-label", "远程文件传输");

  const heading = document.createElement("div");
  heading.className = "transfer-heading";
  const title = document.createElement("strong");
  title.textContent = "文件传输";
  const hint = document.createElement("span");
  hint.textContent = "WebRTC · 32 KiB SHA-256 分块 · 网络重连续传 · 仅 DeskLink 目录";
  heading.append(title, hint);

  const toolbar = document.createElement("div");
  toolbar.className = "transfer-toolbar";
  const choose = document.createElement("button");
  choose.type = "button";
  choose.textContent = "发送文件";
  choose.addEventListener("click", () => fileInput?.click());
  const refresh = document.createElement("button");
  refresh.type = "button";
  refresh.textContent = "刷新远端文件";
  refresh.addEventListener("click", requestRemoteFiles);
  const clear = document.createElement("button");
  clear.type = "button";
  clear.textContent = "清除已结束";
  clear.addEventListener("click", clearFinished);
  toolbar.append(choose, refresh, clear);

  fileInput = document.createElement("input");
  fileInput.type = "file";
  fileInput.multiple = true;
  fileInput.hidden = true;
  fileInput.addEventListener("change", () => {
    if (fileInput?.files) enqueueFiles(fileInput.files);
    if (fileInput) fileInput.value = "";
  });

  const sendLabel = document.createElement("strong");
  sendLabel.className = "transfer-section-label";
  sendLabel.textContent = "发送到远端";
  transferList = document.createElement("div");
  transferList.className = "transfer-list";

  const remoteLabel = document.createElement("strong");
  remoteLabel.className = "transfer-section-label";
  remoteLabel.textContent = "远端 DeskLink 文件";
  remoteFileList = document.createElement("div");
  remoteFileList.className = "remote-file-list";

  downloadProgress = document.createElement("div");
  downloadProgress.className = "download-progress-container";
  downloadProgress.hidden = true;

  transferStatus = document.createElement("span");
  transferStatus.className = "transfer-status";
  transferStatus.textContent = "等待远程文件通道";

  const destination = document.createElement("small");
  destination.className = "transfer-destination";
  destination.textContent = "远端目录：Downloads\\DeskLink（可由管理员覆盖；单文件最多 20 GiB）";

  transferPanel.append(
    heading,
    toolbar,
    fileInput,
    downloadProgress,
    sendLabel,
    transferList,
    remoteLabel,
    remoteFileList,
    transferStatus,
    destination,
  );
  transferButton.addEventListener("click", (event) => {
    event.stopPropagation();
    if (!transferPanel) return;
    const opening = transferPanel.hidden;
    transferPanel.hidden = !opening;
    transferButton?.setAttribute("aria-expanded", opening ? "true" : "false");
    if (opening && channelReady()) requestRemoteFiles();
  });

  wrapper.append(transferButton, transferPanel);
  const clipboard = actions.querySelector(".clipboard-control");
  if (clipboard) actions.insertBefore(wrapper, clipboard);
  else actions.append(wrapper);

  dropOverlay = document.createElement("div");
  dropOverlay.className = "file-drop-overlay";
  dropOverlay.hidden = true;
  dropOverlay.innerHTML = "<strong>发送到远端</strong><span>松开鼠标开始 WebRTC 文件传输</span>";
  stage.append(dropOverlay);

  renderJobs();
  renderRemoteFiles();
}

function hasFiles(event: DragEvent) {
  return Array.from(event.dataTransfer?.types ?? []).includes("Files");
}

function clearDragOverlay() {
  dragDepth = 0;
  if (dropOverlay) dropOverlay.hidden = true;
}

window.addEventListener("desklink:control-channel", (event) => {
  const detail = (event as CustomEvent<ControlChannelDetail>).detail;
  if (detail?.peer) attachPeer(detail.peer);
});

window.addEventListener("dragenter", (event) => {
  if (!sessionActive() || !hasFiles(event)) return;
  event.preventDefault();
  dragDepth += 1;
  if (dropOverlay) dropOverlay.hidden = false;
});

window.addEventListener("dragover", (event) => {
  if (!sessionActive() || !hasFiles(event)) return;
  event.preventDefault();
  if (event.dataTransfer) event.dataTransfer.dropEffect = "copy";
});

window.addEventListener("dragleave", () => {
  if (dragDepth <= 0) return;
  dragDepth -= 1;
  if (dragDepth === 0 && dropOverlay) dropOverlay.hidden = true;
});

window.addEventListener("dragend", clearDragOverlay);

window.addEventListener("drop", (event) => {
  if (!sessionActive() || !hasFiles(event)) return;
  event.preventDefault();
  clearDragOverlay();
  const files = event.dataTransfer?.files;
  if (files?.length) {
    enqueueFiles(files);
    if (transferPanel) transferPanel.hidden = false;
    transferButton?.setAttribute("aria-expanded", "true");
  }
});

window.addEventListener("pointerdown", (event) => {
  const target = event.target;
  if (!(target instanceof Node)) return;
  if (transferPanel && !transferPanel.hidden && !transferPanel.parentElement?.contains(target)) {
    closePanel();
  }
});

window.addEventListener("keydown", (event) => {
  if (event.key === "Escape") closePanel();
});

const observer = new MutationObserver(() => mountTransferControl());
observer.observe(document.documentElement, { subtree: true, childList: true });
mountTransferControl();

export {};
