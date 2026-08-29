const STORAGE_KEY = "desklink.recentDevices.v1";
const MAX_RECENT_DEVICES = 6;
const TARGET_PLACEHOLDERS = new Set(["Remote device ID", "远程设备 ID"]);

function readRecentDevices(): string[] {
  try {
    const parsed = JSON.parse(localStorage.getItem(STORAGE_KEY) ?? "[]");
    if (!Array.isArray(parsed)) return [];
    return parsed
      .filter((value): value is string => typeof value === "string" && value.length > 0 && value.length <= 128)
      .slice(0, MAX_RECENT_DEVICES);
  } catch {
    return [];
  }
}

function writeRecentDevices(values: string[]) {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(values.slice(0, MAX_RECENT_DEVICES)));
  } catch {
    // Private browsing/storage restrictions must never block remote control.
  }
}

function rememberDevice(id: string) {
  const normalized = id.trim();
  if (!normalized || normalized.length > 128) return;
  const next = [normalized, ...readRecentDevices().filter((value) => value !== normalized)];
  writeRecentDevices(next);
}

function targetInput() {
  return document.querySelector<HTMLInputElement>('.connect-card input[data-role="target-device"]')
    ?? Array.from(document.querySelectorAll<HTMLInputElement>(".connect-card input"))
      .find((input) => TARGET_PLACEHOLDERS.has(input.placeholder))
    ?? null;
}

function applyTarget(id: string) {
  const input = targetInput();
  if (!input || input.disabled) return;
  const descriptor = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value");
  descriptor?.set?.call(input, id);
  input.dispatchEvent(new Event("input", { bubbles: true }));
  input.focus();
}

function renderHistory() {
  const card = document.querySelector<HTMLElement>(".connect-card");
  if (!card) return;

  const values = readRecentDevices();
  let row = card.querySelector<HTMLDivElement>(".recent-devices");
  if (values.length === 0) {
    row?.remove();
    return;
  }

  if (!row) {
    row = document.createElement("div");
    row.className = "recent-devices";
    card.append(row);
  }

  const signature = values.join("\n");
  if (row.dataset.signature === signature) return;
  row.dataset.signature = signature;
  row.replaceChildren();

  const label = document.createElement("span");
  label.className = "recent-devices-label";
  label.textContent = "最近设备";
  row.append(label);

  for (const id of values) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "recent-device-chip";
    button.textContent = id;
    button.title = `填入设备 ${id}`;
    button.addEventListener("click", () => applyTarget(id));
    row.append(button);
  }

  const clear = document.createElement("button");
  clear.type = "button";
  clear.className = "recent-devices-clear";
  clear.textContent = "清除";
  clear.title = "只清除本浏览器保存的设备 ID；不会影响远端设备";
  clear.addEventListener("click", () => {
    try {
      localStorage.removeItem(STORAGE_KEY);
    } catch {
    }
    renderHistory();
  });
  row.append(clear);
}

function isConnectButton(button: HTMLButtonElement) {
  if (button.dataset.connectionAction) return button.dataset.connectionAction === "connect";
  const text = button.textContent?.trim();
  return text === "Connect" || text === "连接设备";
}

function bindForm() {
  const card = document.querySelector<HTMLElement>(".connect-card");
  const input = targetInput();
  if (!card || !input || card.dataset.deviceHistoryBound === "1") return;
  card.dataset.deviceHistoryBound = "1";

  card.addEventListener("click", (event) => {
    const button = (event.target as Element | null)?.closest<HTMLButtonElement>("button");
    if (!button || button.closest(".recent-devices") || !isConnectButton(button)) return;
    rememberDevice(input.value);
    renderHistory();
  }, true);

  input.addEventListener("keydown", (event) => {
    if (event.key !== "Enter") return;
    const primary = card.querySelector<HTMLButtonElement>('button[data-connection-action="connect"]');
    if (!primary || primary.disabled) return;
    rememberDevice(input.value);
    renderHistory();
  }, true);
}

function sync() {
  bindForm();
  renderHistory();
}

const root = document.querySelector("#root");
if (root) {
  let queued = false;
  const schedule = () => {
    if (queued) return;
    queued = true;
    requestAnimationFrame(() => {
      queued = false;
      sync();
    });
  };
  new MutationObserver(schedule).observe(root, { childList: true, subtree: true });
  sync();
}
