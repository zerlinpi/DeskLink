type ControlChannelDetail = {
  channel: RTCDataChannel;
};

type ControlMessageDetail = {
  channel: RTCDataChannel;
  message: Record<string, unknown>;
};

type MonitorInfo = {
  index: number;
  name: string;
  width: number;
  height: number;
  primary: boolean;
};

let controlChannel: RTCDataChannel | null = null;
let monitors: MonitorInfo[] = [];
let activeIndex = 0;
let pendingIndex: number | null = null;
let monitorTabs: HTMLDivElement | null = null;
let noteTimer: number | null = null;

function send(payload: Record<string, unknown>) {
  if (controlChannel?.readyState !== "open") return false;
  controlChannel.send(JSON.stringify(payload));
  return true;
}

function parseMonitor(value: unknown): MonitorInfo | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const raw = value as Record<string, unknown>;
  const index = Number(raw.index);
  const width = Number(raw.width);
  const height = Number(raw.height);
  if (!Number.isInteger(index) || index < 0 || index > 63 ||
      !Number.isFinite(width) || width <= 0 ||
      !Number.isFinite(height) || height <= 0) {
    return null;
  }
  return {
    index,
    name: typeof raw.name === "string" ? raw.name : `Display ${index + 1}`,
    width: Math.round(width),
    height: Math.round(height),
    primary: raw.primary === true,
  };
}

function showNote(text: string, error = false) {
  if (!monitorTabs) return;
  let note = monitorTabs.querySelector<HTMLSpanElement>(".monitor-switch-note");
  if (!note) {
    note = document.createElement("span");
    note.className = "monitor-switch-note";
    monitorTabs.append(note);
  }
  note.textContent = text;
  note.classList.toggle("is-error", error);
  if (noteTimer !== null) window.clearTimeout(noteTimer);
  noteTimer = window.setTimeout(() => {
    noteTimer = null;
    note?.remove();
  }, 2500);
}

function renderTabs() {
  if (!monitorTabs) return;
  monitorTabs.querySelectorAll(".monitor-tab").forEach((node) => node.remove());
  monitorTabs.toggleAttribute("hidden", monitors.length <= 1);

  for (const [position, monitor] of monitors.entries()) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "monitor-tab";
    button.dataset.monitorIndex = String(monitor.index);
    button.classList.toggle("is-active", monitor.index === activeIndex);
    button.classList.toggle("is-pending", monitor.index === pendingIndex);
    button.setAttribute("aria-pressed", monitor.index === activeIndex ? "true" : "false");
    button.title = `${monitor.name} · ${monitor.width}×${monitor.height}${monitor.primary ? " · 主屏" : ""}`;

    const label = document.createElement("span");
    label.textContent = `屏幕 ${position + 1}`;
    button.append(label);
    if (monitor.primary) {
      const badge = document.createElement("small");
      badge.textContent = "主屏";
      button.append(badge);
    }

    button.addEventListener("click", () => {
      if (monitor.index === activeIndex || pendingIndex !== null) return;
      if (!send({ t: "monitor-switch", index: monitor.index })) {
        showNote("控制通道未连接", true);
        return;
      }
      pendingIndex = monitor.index;
      renderTabs();
      showNote("正在切换…");
    });
    monitorTabs.insertBefore(button, monitorTabs.querySelector(".monitor-switch-note"));
  }
}

function mountTabs() {
  const toolbar = document.querySelector<HTMLElement>(".remote-workbench");
  const actions = toolbar?.querySelector<HTMLElement>(".workbench-actions");
  if (!toolbar || !actions) return;

  const existing = toolbar.querySelector<HTMLDivElement>(".monitor-tabs");
  if (existing) {
    monitorTabs = existing;
    renderTabs();
    return;
  }

  monitorTabs = document.createElement("div");
  monitorTabs.className = "monitor-tabs";
  monitorTabs.setAttribute("role", "group");
  monitorTabs.setAttribute("aria-label", "远程显示器");
  monitorTabs.hidden = true;
  toolbar.insertBefore(monitorTabs, actions);
  renderTabs();
}

function requestState() {
  send({ t: "monitor-list-request" });
}

function attachControlChannel(channel: RTCDataChannel) {
  controlChannel = channel;
  channel.addEventListener("open", () => {
    if (controlChannel === channel) requestState();
  });
  channel.addEventListener("close", () => {
    if (controlChannel !== channel) return;
    controlChannel = null;
    monitors = [];
    activeIndex = 0;
    pendingIndex = null;
    renderTabs();
  });
  if (channel.readyState === "open") requestState();
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

  if (type === "monitor-state") {
    const values = Array.isArray(message.monitors) ? message.monitors : [];
    const parsed = values.map(parseMonitor).filter((item): item is MonitorInfo => item !== null);
    const nextActive = Number(message.activeIndex);
    if (!Number.isInteger(nextActive) || nextActive < 0 || nextActive > 63) return;
    monitors = parsed.slice(0, 64);
    activeIndex = nextActive;
    pendingIndex = null;
    mountTabs();
    renderTabs();
    return;
  }

  if (type === "monitor-switch-result") {
    const ok = message.ok === true;
    if (!ok) {
      pendingIndex = null;
      renderTabs();
      showNote("切换失败，已保留原屏", true);
    }
  }
});

const observer = new MutationObserver(() => mountTabs());
observer.observe(document.documentElement, { subtree: true, childList: true });
mountTabs();

export {};
