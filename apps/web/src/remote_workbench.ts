type ViewMode = "fit" | "fill" | "actual";

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

let viewMode: ViewMode = "fit";
let networkExpanded = false;
let hideTimer: number | null = null;
let boundStage: HTMLElement | null = null;
let fullscreenButton: HTMLButtonElement | null = null;
let displayButton: HTMLButtonElement | null = null;
let networkButton: HTMLButtonElement | null = null;
let statusText: HTMLSpanElement | null = null;
let toolbar: HTMLDivElement | null = null;

function query<T extends Element>(selector: string): T | null {
  return document.querySelector<T>(selector);
}

function currentStatus() {
  return query<HTMLElement>(STATUS_SELECTOR)?.textContent?.trim() || "idle";
}

function sessionActive() {
  return currentStatus() !== "idle";
}

function friendlyStatus(value: string) {
  if (value === "idle") return "未连接";
  if (value.includes("control ready")) return value.includes("reconnect") ? "已连接 · 信令重连" : "已连接";
  if (value.includes("waiting for host")) return "等待远端上线";
  if (value.includes("host offline")) return "远端离线";
  if (value.includes("reconnecting")) return "正在重连";
  if (value.includes("authorizing")) return "正在验证";
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

function makeButton(label: string, className = "workbench-button") {
  const button = document.createElement("button");
  button.type = "button";
  button.className = className;
  button.textContent = label;
  return button;
}

function applyViewMode(stage: HTMLElement) {
  stage.dataset.viewMode = viewMode;
  if (displayButton) displayButton.textContent = `显示 · ${VIEW_LABELS[viewMode]}`;
}

function syncFullscreenButton() {
  if (!fullscreenButton) return;
  fullscreenButton.textContent = document.fullscreenElement ? "退出全屏" : "全屏";
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

function bindStage(stage: HTMLElement) {
  if (boundStage === stage) return;
  if (boundStage) {
    boundStage.removeEventListener("pointermove", revealToolbar);
    boundStage.removeEventListener("pointerdown", revealToolbar);
  }
  boundStage = stage;
  stage.addEventListener("pointermove", revealToolbar, { passive: true });
  stage.addEventListener("pointerdown", revealToolbar, { passive: true });
}

function createWorkbench(stage: HTMLElement) {
  const existing = stage.querySelector<HTMLDivElement>(".remote-workbench");
  if (existing) {
    toolbar = existing;
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
  displayButton.title = "切换适应窗口、铺满窗口和 1:1 显示";
  displayButton.addEventListener("click", () => {
    const index = VIEW_ORDER.indexOf(viewMode);
    viewMode = VIEW_ORDER[(index + 1) % VIEW_ORDER.length];
    applyViewMode(stage);
    query<HTMLVideoElement>(VIDEO_SELECTOR)?.focus();
    revealToolbar();
  });

  networkButton = makeButton("网络");
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

  fullscreenButton = makeButton("全屏");
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
  actions.append(displayButton, networkButton, focusButton, fullscreenButton, disconnectButton);
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
    networkExpanded = false;
    stage.classList.remove("network-expanded");
    networkButton?.classList.remove("is-active");
    if (hideTimer !== null) window.clearTimeout(hideTimer);
    hideTimer = null;
    bar.classList.remove("is-idle");
  }

  if (statusText) {
    statusText.textContent = friendlyStatus(currentStatus());
    statusText.dataset.state = currentStatus().includes("control ready") ? "ready" : "busy";
  }
  if (networkButton) networkButton.textContent = compactNetworkLabel();
  applyViewMode(stage);
  syncFullscreenButton();
  if (active) revealToolbar();
}

function start() {
  const root = query<HTMLElement>(ROOT_SELECTOR);
  if (!root) return;

  const observer = new MutationObserver(syncWorkbench);
  observer.observe(root, {
    subtree: true,
    childList: true,
    characterData: true,
  });

  document.addEventListener("fullscreenchange", syncFullscreenButton);
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
