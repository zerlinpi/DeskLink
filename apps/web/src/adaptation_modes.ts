import {
  DEFAULT_ADAPTATION_MODE,
  adaptationModeMessage,
  isAdaptationMode,
  type AdaptationMode,
} from "./adaptation_mode";

type ModeOption = {
  id: AdaptationMode;
  label: string;
  detail: string;
};

type ControlChannelDetail = {
  channel: RTCDataChannel;
};

const STORAGE_KEY = "desklink.adaptation-mode";
const MODE_OPTIONS: ModeOption[] = [
  {
    id: "desktop",
    label: "桌面",
    detail: "弱网优先保持文字清晰度与分辨率，再逐级降低帧率",
  },
  {
    id: "game",
    label: "游戏",
    detail: "弱网优先降低码率与分辨率，尽量维持 45 FPS 以上响应",
  },
];

let controlChannel: RTCDataChannel | null = null;
let currentMode: AdaptationMode = loadMode();
let modeButton: HTMLButtonElement | null = null;
let modeMenu: HTMLDivElement | null = null;

function loadMode(): AdaptationMode {
  try {
    const stored = window.localStorage.getItem(STORAGE_KEY);
    return isAdaptationMode(stored) ? stored : DEFAULT_ADAPTATION_MODE;
  } catch {
    return DEFAULT_ADAPTATION_MODE;
  }
}

function saveMode(mode: AdaptationMode) {
  try {
    window.localStorage.setItem(STORAGE_KEY, mode);
  } catch {
    // Persistence is optional; private/locked-down browsers may reject storage.
  }
}

function optionFor(mode: AdaptationMode) {
  return MODE_OPTIONS.find((option) => option.id === mode) ?? MODE_OPTIONS[0];
}

function sendCurrentMode() {
  if (controlChannel?.readyState !== "open") return false;
  controlChannel.send(JSON.stringify(adaptationModeMessage(currentMode)));
  return true;
}

function syncSelection() {
  if (modeButton) {
    modeButton.textContent = `模式 · ${optionFor(currentMode).label}`;
    modeButton.dataset.mode = currentMode;
    modeButton.title = optionFor(currentMode).detail;
  }
  modeMenu?.querySelectorAll<HTMLButtonElement>(".adaptation-option").forEach((button) => {
    const selected = button.dataset.mode === currentMode;
    button.classList.toggle("is-selected", selected);
    button.setAttribute("aria-checked", selected ? "true" : "false");
  });
}

function closeMenu() {
  if (!modeMenu) return;
  modeMenu.hidden = true;
  modeButton?.setAttribute("aria-expanded", "false");
}

function attachControlChannel(channel: RTCDataChannel) {
  controlChannel = channel;
  channel.addEventListener("open", () => {
    if (controlChannel !== channel) return;
    sendCurrentMode();
  });
  channel.addEventListener("close", () => {
    if (controlChannel !== channel) return;
    controlChannel = null;
    closeMenu();
  });
}

window.addEventListener("desklink:control-channel", (event) => {
  const detail = (event as CustomEvent<ControlChannelDetail>).detail;
  if (detail?.channel) attachControlChannel(detail.channel);
});

function selectMode(mode: AdaptationMode) {
  currentMode = mode;
  saveMode(mode);
  syncSelection();
  sendCurrentMode();
  closeMenu();
  document.querySelector<HTMLVideoElement>(".stage video")?.focus();
}

function mountModeControl() {
  const actions = document.querySelector<HTMLElement>(".workbench-actions");
  if (!actions || actions.querySelector(".adaptation-control")) return;

  const wrapper = document.createElement("div");
  wrapper.className = "quality-control adaptation-control";

  modeButton = document.createElement("button");
  modeButton.type = "button";
  modeButton.className = "workbench-button adaptation-trigger";
  modeButton.dataset.workbenchAction = "adaptation";
  modeButton.setAttribute("aria-haspopup", "menu");
  modeButton.setAttribute("aria-expanded", "false");

  modeMenu = document.createElement("div");
  modeMenu.className = "quality-menu adaptation-menu";
  modeMenu.setAttribute("role", "radiogroup");
  modeMenu.setAttribute("aria-label", "远程使用模式");
  modeMenu.hidden = true;

  for (const optionData of MODE_OPTIONS) {
    const option = document.createElement("button");
    option.type = "button";
    option.className = "quality-option adaptation-option";
    option.dataset.mode = optionData.id;
    option.setAttribute("role", "radio");

    const title = document.createElement("strong");
    title.textContent = `${optionData.label}模式`;
    const detail = document.createElement("span");
    detail.textContent = optionData.detail;
    option.append(title, detail);

    option.addEventListener("click", (event) => {
      event.stopPropagation();
      selectMode(optionData.id);
    });
    modeMenu.append(option);
  }

  modeButton.addEventListener("click", (event) => {
    event.stopPropagation();
    if (!modeMenu) return;
    const opening = modeMenu.hidden;
    modeMenu.hidden = !opening;
    modeButton?.setAttribute("aria-expanded", opening ? "true" : "false");
  });

  wrapper.append(modeButton, modeMenu);
  const qualityControl = actions.querySelector(".quality-control:not(.adaptation-control)");
  if (qualityControl) actions.insertBefore(wrapper, qualityControl);
  else actions.append(wrapper);
  syncSelection();
}

const observer = new MutationObserver(() => mountModeControl());
observer.observe(document.documentElement, { subtree: true, childList: true });

window.addEventListener("pointerdown", (event) => {
  const target = event.target;
  if (!(target instanceof Node)) return;
  if (modeMenu && !modeMenu.hidden && !modeMenu.parentElement?.contains(target)) closeMenu();
});
window.addEventListener("keydown", (event) => {
  if (event.key === "Escape") closeMenu();
});

mountModeControl();

export {};
