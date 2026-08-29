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

const CHUNK_BYTES = 32 * 1024;
const CHUNK_HEADER_BYTES = 8 + 32;
const BUFFER_LOW_BYTES = 512 * 1024;
const BUFFER_HIGH_BYTES = 2 * 1024 * 1024;
const MAX_TRANSFER_BYTES = 20 * 1024 * 1024 * 1024;

let peer: RTCPeerConnection | null = null;
let transferChannel: RTCDataChannel | null = null;
let activeJob: UploadJob | null = null;
const jobs: UploadJob[] = [];

let transferButton: HTMLButtonElement | null = null;
let transferPanel: HTMLDivElement | null = null;
let transferList: HTMLDivElement | null = null;
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
    const pending = jobs.filter((job) => !["complete", "cancelled", "error"].includes(job.state)).length;
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

function attachTransferChannel(channel: RTCDataChannel) {
  if (transferChannel && transferChannel !== channel && transferChannel.readyState !== "closed") {
    try { transferChannel.close(); } catch { /* ignored */ }
  }
  transferChannel = channel;
  channel.binaryType = "arraybuffer";
  channel.bufferedAmountLowThreshold = BUFFER_LOW_BYTES;

  channel.addEventListener("open", () => {
    if (transferChannel !== channel) return;
    setStatus("WebRTC 文件通道已就绪");
    pumpQueue();
  });

  channel.addEventListener("message", (event) => {
    if (transferChannel !== channel || typeof event.data !== "string" || event.data.length > 16 * 1024) return;
    try {
      const parsed = JSON.parse(event.data) as unknown;
      if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) return;
      handleUploadMessage(parsed as UploadMessage);
    } catch {
      // Binary chunks are controller -> host only; host responses are compact JSON.
    }
  });

  channel.addEventListener("close", () => {
    if (transferChannel !== channel) return;
    transferChannel = null;
    if (activeJob && !["complete", "cancelled", "error"].includes(activeJob.state)) {
      activeJob.streamToken += 1;
      activeJob.state = "paused";
      setStatus("文件通道已断开，重连后自动从远端确认位置续传");
      renderJobs();
    }
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
  renderJobs();
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
  transferButton.title = "WebRTC 分块发送文件到远端 Windows，网络重连可续传";

  transferPanel = document.createElement("div");
  transferPanel.className = "file-transfer-panel";
  transferPanel.hidden = true;
  transferPanel.setAttribute("role", "dialog");
  transferPanel.setAttribute("aria-label", "发送文件到远端");

  const heading = document.createElement("div");
  heading.className = "transfer-heading";
  const title = document.createElement("strong");
  title.textContent = "发送文件到远端";
  const hint = document.createElement("span");
  hint.textContent = "WebRTC · 32 KiB SHA-256 分块 · 网络重连自动续传";
  heading.append(title, hint);

  const toolbar = document.createElement("div");
  toolbar.className = "transfer-toolbar";
  const choose = document.createElement("button");
  choose.type = "button";
  choose.textContent = "选择文件";
  choose.addEventListener("click", () => fileInput?.click());
  const clear = document.createElement("button");
  clear.type = "button";
  clear.textContent = "清除已结束";
  clear.addEventListener("click", clearFinished);
  toolbar.append(choose, clear);

  fileInput = document.createElement("input");
  fileInput.type = "file";
  fileInput.multiple = true;
  fileInput.hidden = true;
  fileInput.addEventListener("change", () => {
    if (fileInput?.files) enqueueFiles(fileInput.files);
    if (fileInput) fileInput.value = "";
  });

  transferList = document.createElement("div");
  transferList.className = "transfer-list";

  transferStatus = document.createElement("span");
  transferStatus.className = "transfer-status";
  transferStatus.textContent = "等待远程文件通道";

  const destination = document.createElement("small");
  destination.className = "transfer-destination";
  destination.textContent = "远端默认保存：Downloads\\DeskLink（单文件最多 20 GiB）";

  transferPanel.append(heading, toolbar, fileInput, transferList, transferStatus, destination);
  transferButton.addEventListener("click", (event) => {
    event.stopPropagation();
    if (!transferPanel) return;
    const opening = transferPanel.hidden;
    transferPanel.hidden = !opening;
    transferButton?.setAttribute("aria-expanded", opening ? "true" : "false");
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
