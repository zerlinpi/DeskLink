import {
  resetClipboardRequestsForChannelChange,
  type ClipboardPendingDirection,
} from "./clipboard_request_scope";

type ControlChannelDetail = {
  channel: RTCDataChannel;
};

type ControlMessageDetail = {
  channel: RTCDataChannel;
  message: Record<string, unknown>;
};

const MAX_CLIPBOARD_UTF8_BYTES = 128 * 1024;
const encoder = new TextEncoder();
let controlChannel: RTCDataChannel | null = null;
let clipboardButton: HTMLButtonElement | null = null;
let clipboardPanel: HTMLDivElement | null = null;
let clipboardText: HTMLTextAreaElement | null = null;
let clipboardStatus: HTMLSpanElement | null = null;
const pending = new Map<string, ClipboardPendingDirection>();

function textBytes(value: string) {
  return encoder.encode(value).byteLength;
}

function setStatus(value: string, error = false) {
  if (!clipboardStatus) return;
  clipboardStatus.textContent = value;
  clipboardStatus.classList.toggle("is-error", error);
}

function send(payload: Record<string, unknown>) {
  if (controlChannel?.readyState !== "open") {
    setStatus("控制通道未连接", true);
    return false;
  }
  controlChannel.send(JSON.stringify(payload));
  return true;
}

function closePanel() {
  if (!clipboardPanel) return;
  clipboardPanel.hidden = true;
  clipboardButton?.setAttribute("aria-expanded", "false");
}

function requestId() {
  return crypto.randomUUID();
}

function sendTextToRemote(value: string) {
  const bytes = textBytes(value);
  if (bytes > MAX_CLIPBOARD_UTF8_BYTES) {
    setStatus(`文本过大：${Math.ceil(bytes / 1024)} KiB，最多 128 KiB`, true);
    return;
  }
  const id = requestId();
  if (!send({ t: "clipboard-write", requestId: id, text: value })) return;
  pending.set(id, "local-to-remote");
  setStatus("正在写入远端剪贴板…");
}

async function readLocalClipboard() {
  if (!clipboardText) return;
  try {
    if (!navigator.clipboard?.readText) throw new Error("Clipboard API unavailable");
    const value = await navigator.clipboard.readText();
    if (textBytes(value) > MAX_CLIPBOARD_UTF8_BYTES) {
      setStatus("本地剪贴板文本超过 128 KiB", true);
      return;
    }
    clipboardText.value = value;
    setStatus("已读取本地剪贴板，可发送到远端");
  } catch (error) {
    console.debug("DeskLink local clipboard read unavailable", error);
    setStatus("浏览器未授权读取，请在文本框中手动粘贴", true);
    clipboardText.focus();
  }
}

function requestRemoteClipboard() {
  const id = requestId();
  if (!send({ t: "clipboard-read-request", requestId: id })) return;
  pending.set(id, "remote-to-local");
  setStatus("正在读取远端剪贴板…");
}

async function copyTextLocally() {
  if (!clipboardText) return;
  try {
    if (!navigator.clipboard?.writeText) throw new Error("Clipboard API unavailable");
    await navigator.clipboard.writeText(clipboardText.value);
    setStatus("已复制到本地剪贴板");
  } catch (error) {
    console.debug("DeskLink local clipboard write unavailable", error);
    clipboardText.focus();
    clipboardText.select();
    setStatus("浏览器未授权写入，文本已选中，请按 Ctrl+C", true);
  }
}

function attachControlChannel(channel: RTCDataChannel) {
  const cancelledPending = resetClipboardRequestsForChannelChange(
    pending,
    controlChannel,
    channel,
  );
  controlChannel = channel;

  if (cancelledPending) {
    setStatus("控制通道已切换，未完成的剪贴板操作已取消");
  }
  if (channel.readyState === "open") {
    setStatus(cancelledPending ? "剪贴板已恢复，请重新执行操作" : "剪贴板就绪");
  }

  channel.addEventListener("open", () => {
    if (controlChannel === channel) setStatus("剪贴板就绪");
  });
  channel.addEventListener("close", () => {
    if (controlChannel !== channel) return;
    controlChannel = null;
    pending.clear();
    setStatus("远程会话已断开");
    closePanel();
  });
}

function mountClipboard() {
  const actions = document.querySelector<HTMLElement>(".workbench-actions");
  if (!actions || actions.querySelector(".clipboard-control")) return;

  const wrapper = document.createElement("div");
  wrapper.className = "clipboard-control";

  clipboardButton = document.createElement("button");
  clipboardButton.type = "button";
  clipboardButton.className = "workbench-button clipboard-trigger";
  clipboardButton.textContent = "剪贴板";
  clipboardButton.setAttribute("aria-haspopup", "dialog");
  clipboardButton.setAttribute("aria-expanded", "false");
  clipboardButton.title = "显式读取/发送文本剪贴板，不会后台自动同步";

  clipboardPanel = document.createElement("div");
  clipboardPanel.className = "clipboard-panel";
  clipboardPanel.hidden = true;
  clipboardPanel.setAttribute("role", "dialog");
  clipboardPanel.setAttribute("aria-label", "远程文本剪贴板");

  const heading = document.createElement("div");
  heading.className = "clipboard-heading";
  const title = document.createElement("strong");
  title.textContent = "文本剪贴板";
  const hint = document.createElement("span");
  hint.textContent = "P2P 传输 · 不自动同步 · 单次最多 128 KiB";
  heading.append(title, hint);

  clipboardText = document.createElement("textarea");
  clipboardText.className = "clipboard-text";
  clipboardText.rows = 7;
  clipboardText.placeholder = "在这里粘贴要发送到远端的文字，或读取远端剪贴板…";
  clipboardText.spellcheck = false;

  const actionsRow = document.createElement("div");
  actionsRow.className = "clipboard-actions";

  const readLocal = document.createElement("button");
  readLocal.type = "button";
  readLocal.textContent = "读取本地";
  readLocal.addEventListener("click", () => void readLocalClipboard());

  const sendRemote = document.createElement("button");
  sendRemote.type = "button";
  sendRemote.textContent = "发送远端";
  sendRemote.addEventListener("click", () => {
    if (clipboardText) sendTextToRemote(clipboardText.value);
  });

  const readRemote = document.createElement("button");
  readRemote.type = "button";
  readRemote.textContent = "读取远端";
  readRemote.addEventListener("click", requestRemoteClipboard);

  const copyLocal = document.createElement("button");
  copyLocal.type = "button";
  copyLocal.textContent = "复制本地";
  copyLocal.addEventListener("click", () => void copyTextLocally());

  actionsRow.append(readLocal, sendRemote, readRemote, copyLocal);

  clipboardStatus = document.createElement("span");
  clipboardStatus.className = "clipboard-status";
  clipboardStatus.textContent = "剪贴板就绪";

  clipboardPanel.append(heading, clipboardText, actionsRow, clipboardStatus);
  clipboardButton.addEventListener("click", (event) => {
    event.stopPropagation();
    if (!clipboardPanel) return;
    const opening = clipboardPanel.hidden;
    clipboardPanel.hidden = !opening;
    clipboardButton?.setAttribute("aria-expanded", opening ? "true" : "false");
    if (opening) clipboardText?.focus();
  });

  wrapper.append(clipboardButton, clipboardPanel);
  const fullscreen = actions.querySelector('[data-workbench-action="fullscreen"]');
  if (fullscreen) actions.insertBefore(wrapper, fullscreen);
  else actions.append(wrapper);
}

window.addEventListener("desklink:control-channel", (event) => {
  const detail = (event as CustomEvent<ControlChannelDetail>).detail;
  if (detail?.channel) attachControlChannel(detail.channel);
});

window.addEventListener("desklink:control-message", (event) => {
  const detail = (event as CustomEvent<ControlMessageDetail>).detail;
  if (!detail || detail.channel !== controlChannel) return;
  const message = detail.message;
  const type = message.t;
  const id = typeof message.requestId === "string" ? message.requestId : "";

  if (type === "clipboard-text" && id && pending.get(id) === "remote-to-local") {
    pending.delete(id);
    const value = typeof message.text === "string" ? message.text : "";
    if (textBytes(value) > MAX_CLIPBOARD_UTF8_BYTES) {
      setStatus("远端返回的文本超过 128 KiB，已拒绝", true);
      return;
    }
    if (clipboardText) clipboardText.value = value;
    setStatus("已读取远端剪贴板；如需本地使用，请点击“复制本地”");
    return;
  }

  if (type === "clipboard-result" && id && pending.has(id)) {
    const direction = pending.get(id);
    pending.delete(id);
    if (message.ok === true) {
      setStatus(direction === "local-to-remote" ? "已写入远端剪贴板" : "剪贴板操作完成");
    } else {
      const error = typeof message.error === "string" ? message.error : "远端剪贴板操作失败";
      setStatus(error, true);
    }
  }
});

window.addEventListener("pointerdown", (event) => {
  const target = event.target;
  if (!(target instanceof Node)) return;
  if (clipboardPanel && !clipboardPanel.hidden && !clipboardPanel.parentElement?.contains(target)) {
    closePanel();
  }
});

window.addEventListener("keydown", (event) => {
  if (event.key === "Escape") closePanel();
});

const observer = new MutationObserver(() => mountClipboard());
observer.observe(document.documentElement, { subtree: true, childList: true });
mountClipboard();

export {};
