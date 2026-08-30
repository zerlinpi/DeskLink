type ViewMode = "fit" | "fill" | "actual";

type HostCapabilities = {
  version: number;
  secureAttentionAvailable: boolean;
  secureAttentionReason: string;
  secureAttentionPolicy: string;
  clipboardAvailable: boolean;
  fileTransferAvailable: boolean;
  audioAvailable: boolean;
  protectedDesktopAvailable: boolean;
};

const VIEW_LABELS: Record<ViewMode, string> = {
  fit: "适应",
  fill: "铺满",
  actual: "1:1",
};

const VIEW_ORDER: ViewMode[] = ["fit", "fill", "actual"];
const ROOT_SELECTOR = "#root";
const STAGE_SELECTOR = ".stage";
const STATUS_SELECTOR = ".status";
const CONNECT_BUTTON_SELECTOR = ".connect-card button";
const VIDEO_SELECTOR = ".stage video";
const NETWORK_SELECTOR = ".network-hud";
const SAS_OPERATION = "secure-attention-sequence";
const SAS_TIMEOUT_MS = 6000;

let viewMode: ViewMode = "fit";
let networkExpanded = false;
let hideTimer: number | null = null;
let boundStage: HTMLElement | null = null;
let boundVideo: HTMLVideoElement | null = null;
let stageResizeObserver: ResizeObserver | null = null;
let fullscreenButton: HTMLButtonElement | null = null;
let displayButton: HTMLButtonElement | null = null;
let networkButton: HTMLButtonElement | null = null;
let secureAttentionButton: HTMLButtonElement | null = null;
let statusText: HTMLSpanElement | null = null;
let toolbar: HTMLDivElement | null = null;
let controlChannel: RTCDataChannel | null = null;
let hostCapabilities: HostCapabilities | null = null;
let secureAttentionRequestId = "";
let secureAttentionTimer: number | null = null;
let secureAttentionResetTimer: number | null = null;

function query<T extends Element>(selector: string): T | null {
  return document.querySelector<T>(selector);
}

function currentStatus() {
  return query<HTMLElement>(STATUS_SELECTOR)?.textContent?.trim() || "idle";
}

function sessionActive() {
  const value = currentStatus().toLowerCase();
  if (["new", "connecting", "connected", "disconnected", "checking"].includes(value)) return true;
  return [
    "signaling",
    "authorizing",
    "proving host access",
    "negotiating",
    "reconnecting",
    "control ready",
    "host offline",
  ].some((marker) => value.includes(marker));
}

function friendlyStatus(value: string) {
  if (value === "idle") return "未连接";
  if (value.includes("control ready")) return value.includes("reconnect") ? "已连接 · 信令重连" : "已连接";
  if (value.includes("waiting for host")) return "等待远端上线";
  if (value.includes("host offline")) return "远端离线";
  if (value.includes("reconnecting")) return "正在重连";
  if (value.includes("authorizing")) return "正在验证";
  if (value.includes("proving host access")) return "正在验证访问码";
  if (value.includes("negotiating")) return "正在建立画面";
  if (value.includes("device revoked")) return "设备已撤销";
  if (value.includes("rejected")) return "连接被拒绝";
  if (value.includes("error")) return "连接异常";
  return value;
}

function compactNetworkLabel() {
  const hud = query<HTMLElement>(NETWORK_SELECTOR);
  if (!hud) return "网络";
  const spans = Array.from(hud.querySelectorAll("span"));
  const route = spans[0]?.textContent?.trim();
  const rtt = spans.find((node) => node.textContent?.startsWith("RTT "))?.textContent?.replace("RTT ", "").trim();
  if (rtt && rtt !== "-") return `网络 · ${rtt}`;
  return route ? `网络 · ${route}` : "网络";
}

function setText(element: HTMLElement | null, value: string) {
  if (element && element.textContent !== value) element.textContent = value;
}

function readBoolean(message: Record<string, unknown>, key: string) {
  return message[key] === true;
}

function parseHostCapabilities(message: Record<string, unknown>): HostCapabilities | null {
  if (message.t !== "host-capabilities") return null;
  const version = typeof message.version === "number" ? message.version : 0;
  if (version < 1) return null;
  return {
    version,
    secureAttentionAvailable: readBoolean(message, "secureAttentionAvailable"),
    secureAttentionReason: typeof message.secureAttentionReason === "string" ? message.secureAttentionReason : "",
    secureAttentionPolicy: typeof message.secureAttentionPolicy === "string" ? message.secureAttentionPolicy : "unknown",
    clipboardAvailable: readBoolean(message, "clipboardAvailable"),
    fileTransferAvailable: readBoolean(message, "fileTransferAvailable"),
    audioAvailable: readBoolean(message, "audioAvailable"),
    protectedDesktopAvailable: readBoolean(message, "protectedDesktopAvailable"),
  };
}

function secureAttentionUnavailableLabel() {
  const reason = hostCapabilities?.secureAttentionReason || "";
  if (reason === "service-broker-unavailable") return "Service 不支持";
  if (reason === "policy-not-allowed") return "策略未允许";
  if (reason === "api-unavailable") return "系统不支持";
  if (reason === "policy-read-error") return "策略不可读";
  return "Ctrl+Alt+Del";
}

function secureAttentionUnavailableTitle() {
  if (!hostCapabilities) return "等待远端上报系统操作能力";
  switch (hostCapabilities.secureAttentionReason) {
    case "service-broker-unavailable":
      return "被控端 Service 未启用 Secure Attention Broker";
    case "policy-not-allowed":
      return `Windows 本地策略未允许 Services 发送软件安全注意序列（${hostCapabilities.secureAttentionPolicy}）`;
    case "api-unavailable":
      return "被控端 Windows Secure Attention Sequence API 不可用";
    case "policy-read-error":
      return "被控端无法读取 SoftwareSASGeneration 策略，已安全禁用该操作";
    default:
      return "被控端未提供 Ctrl+Alt+Del 能力";
  }
}

function refreshSecureAttentionButton() {
  if (!secureAttentionButton) return;
  const available = hostCapabilities?.secureAttentionAvailable === true;
  const channelReady = controlChannel?.readyState === "open";
  secureAttentionButton.disabled = !sessionActive() || !channelReady || !available || Boolean(secureAttentionRequestId);
  if (!secureAttentionRequestId) {
    setText(secureAttentionButton, available ? "Ctrl+Alt+Del" : secureAttentionUnavailableLabel());
  }
  secureAttentionButton.title = available
    ? "向被控端 Windows Service 请求安全注意序列 Ctrl+Alt+Del"
    : secureAttentionUnavailableTitle();
}

function resetSecureAttentionLabel(delayMs = 0) {
  if (secureAttentionResetTimer !== null) window.clearTimeout(secureAttentionResetTimer);
  secureAttentionResetTimer = null;
  const reset = () => {
    secureAttentionResetTimer = null;
    refreshSecureAttentionButton();
  };
  if (delayMs > 0) {
    secureAttentionResetTimer = window.setTimeout(reset, delayMs);
  } else {
    reset();
  }
}

function finishSecureAttention(label: string) {
  secureAttentionRequestId = "";
  if (secureAttentionTimer !== null) window.clearTimeout(secureAttentionTimer);
  secureAttentionTimer = null;
  setText(secureAttentionButton, label);
  if (secureAttentionButton) secureAttentionButton.disabled = true;
  resetSecureAttentionLabel(2200);
}

function clearSecureAttentionState(clearCapability = false) {
  secureAttentionRequestId = "";
  if (secureAttentionTimer !== null) window.clearTimeout(secureAttentionTimer);
  if (secureAttentionResetTimer !== null) window.clearTimeout(secureAttentionResetTimer);
  secureAttentionTimer = null;
  secureAttentionResetTimer = null;
  if (clearCapability) hostCapabilities = null;
  refreshSecureAttentionButton();
}

function requestSecureAttention() {
  if (!sessionActive() ||
      controlChannel?.readyState !== "open" ||
      hostCapabilities?.secureAttentionAvailable !== true ||
      secureAttentionRequestId) {
    return;
  }

  if (secureAttentionResetTimer !== null) window.clearTimeout(secureAttentionResetTimer);
  secureAttentionResetTimer = null;
  const requestId = `sas-${crypto.randomUUID()}`;
  secureAttentionRequestId = requestId;
  setText(secureAttentionButton, "请求中…");
  refreshSecureAttentionButton();

  try {
    controlChannel.send(JSON.stringify({
      t: "system-operation",
      operation: SAS_OPERATION,
      requestId,
    }));
  } catch (error) {
    console.debug("DeskLink Secure Attention request failed", error);
    finishSecureAttention("发送失败");
    return;
  }

  secureAttentionTimer = window.setTimeout(() => {
    if (secureAttentionRequestId === requestId) finishSecureAttention("请求超时");
  }, SAS_TIMEOUT_MS);
}

function handleSystemOperationResult(message: Record<string, unknown>) {
  if (message.t !== "system-operation-result" ||
      message.operation !== SAS_OPERATION ||
      message.requestId !== secureAttentionRequestId) {
    return;
  }

  if (message.ok === true) {
    finishSecureAttention("已发送");
    return;
  }

  const errorCode = typeof message.errorCode === "string" ? message.errorCode : "";
  const errorText = typeof message.error === "string" ? message.error.toLowerCase() : "";
  if (errorCode === "service-broker-unavailable" || errorText.includes("broker is not enabled")) {
    finishSecureAttention("Service 不支持");
  } else if (errorCode === "policy-not-allowed" || errorText.includes("policy does not allow services")) {
    finishSecureAttention("策略未允许");
  } else if (errorCode === "api-unavailable" || errorText.includes("api is unavailable")) {
    finishSecureAttention("系统不支持");
  } else if (errorCode === "rate-limited" || errorText.includes("rate limited")) {
    finishSecureAttention("操作过快");
  } else {
    console.debug("DeskLink Secure Attention unavailable", message.error ?? "unknown error");
    finishSecureAttention("当前不可用");
  }
}

function makeButton(label: string, className = "workbench-button") {
  const button = document.createElement("button");
  button.type = "button";
  button.className = className;
  button.textContent = label;
  return button;
}

function updateVideoScale(stage: HTMLElement) {
  const video = query<HTMLVideoElement>(VIDEO_SELECTOR);
  if (!video || video.videoWidth <= 0 || video.videoHeight <= 0 || stage.clientWidth <= 0 || stage.clientHeight <= 0) {
    video?.style.setProperty("--desklink-video-scale", "1");
    return;
  }

  const containScale = Math.min(
    stage.clientWidth / video.videoWidth,
    stage.clientHeight / video.videoHeight,
  );
  if (!Number.isFinite(containScale) || containScale <= 0) return;

  let requestedScale = containScale;
  if (viewMode === "fill") {
    requestedScale = Math.max(
      stage.clientWidth / video.videoWidth,
      stage.clientHeight / video.videoHeight,
    );
  } else if (viewMode === "actual") {
    requestedScale = 1;
  }

  const elementScale = Math.max(0.1, Math.min(8, requestedScale / containScale));
  video.style.setProperty("--desklink-video-scale", elementScale.toFixed(6));
}

function applyViewMode(stage: HTMLElement) {
  if (stage.dataset.viewMode !== viewMode) stage.dataset.viewMode = viewMode;
  updateVideoScale(stage);
  setText(displayButton, `显示 · ${VIEW_LABELS[viewMode]}`);
}

function syncFullscreenButton() {
  setText(fullscreenButton, document.fullscreenElement ? "退出全屏" : "全屏");
  if (boundStage) updateVideoScale(boundStage);
}

function revealToolbar() {
  if (!toolbar) return;
  toolbar.classList.remove("is-idle");
  if (hideTimer !== null) window.clearTimeout(hideTimer);
  if (!sessionActive() || networkExpanded) {
    hideTimer = null;
    return;
  }
  hideTimer = window.setTimeout(() => {
    hideTimer = null;
    toolbar?.classList.add("is-idle");
  }, 3500);
}

function onVideoGeometryChanged() {
  if (boundStage) updateVideoScale(boundStage);
}

function bindStage(stage: HTMLElement) {
  const video = query<HTMLVideoElement>(VIDEO_SELECTOR);
  if (boundStage !== stage) {
    if (boundStage) {
      boundStage.removeEventListener("pointermove", revealToolbar);
      boundStage.removeEventListener("pointerdown", revealToolbar);
    }
    stageResizeObserver?.disconnect();
    stageResizeObserver = new ResizeObserver(() => updateVideoScale(stage));
    stageResizeObserver.observe(stage);
    boundStage = stage;
    stage.addEventListener("pointermove", revealToolbar, { passive: true });
    stage.addEventListener("pointerdown", revealToolbar, { passive: true });
  }

  if (boundVideo !== video) {
    if (boundVideo) {
      boundVideo.removeEventListener("loadedmetadata", onVideoGeometryChanged);
      boundVideo.removeEventListener("resize", onVideoGeometryChanged);
    }
    boundVideo = video;
    if (video) {
      video.addEventListener("loadedmetadata", onVideoGeometryChanged);
      video.addEventListener("resize", onVideoGeometryChanged);
    }
  }
}

function createWorkbench(stage: HTMLElement) {
  const existing = stage.querySelector<HTMLDivElement>(".remote-workbench");
  if (existing) {
    toolbar = existing;
    statusText = existing.querySelector<HTMLSpanElement>(".workbench-status");
    displayButton = existing.querySelector<HTMLButtonElement>("[data-workbench-action='display']");
    networkButton = existing.querySelector<HTMLButtonElement>("[data-workbench-action='network']");
    secureAttentionButton = existing.querySelector<HTMLButtonElement>("[data-workbench-action='secure-attention']");
    fullscreenButton = existing.querySelector<HTMLButtonElement>("[data-workbench-action='fullscreen']");
    refreshSecureAttentionButton();
    return existing;
  }

  const bar = document.createElement("div");
  bar.className = "remote-workbench";
  bar.setAttribute("role", "toolbar");
  bar.setAttribute("aria-label", "远程控制工具栏");

  const brand = document.createElement("div");
  brand.className = "workbench-brand";
  brand.innerHTML = `<strong>DeskLink</strong><span class="workbench-status">未连接</span>`;
  statusText = brand.querySelector(".workbench-status");

  displayButton = makeButton("显示 · 适应");
  displayButton.dataset.workbenchAction = "display";
  displayButton.title = "切换适应窗口、铺满窗口和 1:1 显示";
  displayButton.addEventListener("click", () => {
    const index = VIEW_ORDER.indexOf(viewMode);
    viewMode = VIEW_ORDER[(index + 1) % VIEW_ORDER.length];
    applyViewMode(stage);
    query<HTMLVideoElement>(VIDEO_SELECTOR)?.focus();
    revealToolbar();
  });

  networkButton = makeButton("网络");
  networkButton.dataset.workbenchAction = "network";
  networkButton.title = "查看实时延迟、丢包、抖动、帧率和可用带宽";
  networkButton.addEventListener("click", () => {
    networkExpanded = !networkExpanded;
    stage.classList.toggle("network-expanded", networkExpanded);
    networkButton?.classList.toggle("is-active", networkExpanded);
    revealToolbar();
  });

  const focusButton = makeButton("键鼠捕获");
  focusButton.title = "将键盘焦点交给远程桌面";
  focusButton.addEventListener("click", () => {
    query<HTMLVideoElement>(VIDEO_SELECTOR)?.focus();
    revealToolbar();
  });

  secureAttentionButton = makeButton("Ctrl+Alt+Del");
  secureAttentionButton.dataset.workbenchAction = "secure-attention";
  secureAttentionButton.setAttribute("aria-label", "向远程 Windows 请求 Ctrl+Alt+Del");
  secureAttentionButton.addEventListener("click", () => {
    requestSecureAttention();
    revealToolbar();
  });
  refreshSecureAttentionButton();

  fullscreenButton = makeButton("全屏");
  fullscreenButton.dataset.workbenchAction = "fullscreen";
  fullscreenButton.addEventListener("click", async () => {
    try {
      if (document.fullscreenElement) {
        await document.exitFullscreen();
      } else {
        await stage.requestFullscreen({ navigationUI: "hide" });
      }
    } catch (error) {
      console.debug("DeskLink fullscreen request failed", error);
    }
    syncFullscreenButton();
    revealToolbar();
  });

  const disconnectButton = makeButton("断开", "workbench-button workbench-danger");
  disconnectButton.addEventListener("click", () => {
    const button = query<HTMLButtonElement>(CONNECT_BUTTON_SELECTOR);
    if (button && sessionActive()) button.click();
  });

  const actions = document.createElement("div");
  actions.className = "workbench-actions";
  actions.append(displayButton, networkButton, focusButton, secureAttentionButton, fullscreenButton, disconnectButton);
  bar.append(brand, actions);
  stage.append(bar);
  toolbar = bar;
  applyViewMode(stage);
  syncFullscreenButton();
  return bar;
}

function syncWorkbench() {
  const stage = query<HTMLElement>(STAGE_SELECTOR);
  if (!stage) return;

  bindStage(stage);
  const bar = createWorkbench(stage);
  const active = sessionActive();
  document.body.classList.toggle("remote-session-active", active);
  bar.toggleAttribute("hidden", !active);

  if (!active) {
    clearSecureAttentionState(true);
    controlChannel = null;
    networkExpanded = false;
    stage.classList.remove("network-expanded");
    networkButton?.classList.remove("is-active");
    if (hideTimer !== null) window.clearTimeout(hideTimer);
    hideTimer = null;
    bar.classList.remove("is-idle");
    viewMode = "fit";
  }

  const rawStatus = currentStatus();
  if (statusText) {
    setText(statusText, friendlyStatus(rawStatus));
    const state = rawStatus.includes("control ready") ? "ready" : "busy";
    if (statusText.dataset.state !== state) statusText.dataset.state = state;
  }
  setText(networkButton, compactNetworkLabel());
  refreshSecureAttentionButton();
  applyViewMode(stage);
  syncFullscreenButton();
  if (active) revealToolbar();
}

function start() {
  const root = query<HTMLElement>(ROOT_SELECTOR);
  if (!root) return;

  let syncQueued = false;
  const scheduleSync = () => {
    if (syncQueued) return;
    syncQueued = true;
    requestAnimationFrame(() => {
      syncQueued = false;
      syncWorkbench();
    });
  };

  const observer = new MutationObserver(scheduleSync);
  observer.observe(root, {
    subtree: true,
    childList: true,
    characterData: true,
  });

  document.addEventListener("fullscreenchange", syncFullscreenButton);
  window.addEventListener("desklink:control-channel", (event) => {
    const detail = (event as CustomEvent<{ channel: RTCDataChannel }>).detail;
    const nextChannel = detail?.channel ?? null;
    if (nextChannel !== controlChannel) {
      clearSecureAttentionState(true);
      controlChannel = nextChannel;
    }
    if (controlChannel) {
      controlChannel.addEventListener("open", refreshSecureAttentionButton, { once: true });
    }
    refreshSecureAttentionButton();
  });
  window.addEventListener("desklink:control-channel-closed", (event) => {
    const detail = (event as CustomEvent<{ channel: RTCDataChannel }>).detail;
    if (detail?.channel && detail.channel !== controlChannel) return;
    controlChannel = null;
    hostCapabilities = null;
    if (secureAttentionRequestId) {
      finishSecureAttention("连接已断开");
    } else {
      clearSecureAttentionState(true);
    }
  });
  window.addEventListener("desklink:control-message", (event) => {
    const detail = (event as CustomEvent<{
      channel: RTCDataChannel;
      message: Record<string, unknown>;
    }>).detail;
    if (!detail?.message || detail.channel !== controlChannel) return;

    const capabilities = parseHostCapabilities(detail.message);
    if (capabilities) {
      hostCapabilities = capabilities;
      if (!capabilities.secureAttentionAvailable && secureAttentionRequestId) {
        finishSecureAttention(secureAttentionUnavailableLabel());
      } else {
        refreshSecureAttentionButton();
      }
      return;
    }
    handleSystemOperationResult(detail.message);
  });
  window.addEventListener("keydown", (event) => {
    if (!sessionActive()) return;
    if (event.key === "Escape" && networkExpanded && !document.fullscreenElement) {
      networkExpanded = false;
      query<HTMLElement>(STAGE_SELECTOR)?.classList.remove("network-expanded");
      networkButton?.classList.remove("is-active");
      revealToolbar();
    }
  });

  syncWorkbench();
}

start();
