const STATUS_SELECTOR = ".status";
const CARD_SELECTOR = ".connect-card";

function friendlyStatus(value: string) {
  const normalized = value.trim().toLowerCase();
  if (!normalized || normalized === "idle") return "未连接";
  if (normalized.includes("control ready")) return "已连接";
  if (normalized.includes("authorizing controller")) return "正在验证控制端";
  if (normalized.includes("authorizing host")) return "正在验证远端";
  if (normalized.includes("proving host access")) return "正在验证访问码";
  if (normalized.includes("negotiating")) return "正在建立远程画面";
  if (normalized.includes("reconnecting network")) return "网络恢复中";
  if (normalized.includes("reconnecting signaling")) return "信令重连中";
  if (normalized.includes("waiting for host")) return "等待远端上线";
  if (normalized.includes("host offline")) return "远端离线";
  if (normalized.includes("device revoked")) return "设备已撤销";
  if (normalized.includes("access code rejected")) return "访问码错误";
  if (normalized.includes("authorization rejected")) return "授权被拒绝";
  if (normalized.includes("turn credential error")) return "TURN 凭证异常";
  if (normalized.includes("signal error")) return "信令连接异常";
  if (normalized.includes("error") || normalized.includes("failed")) return "连接异常";
  return value;
}

function setText(element: Element | null, value: string) {
  if (element && element.textContent !== value) element.textContent = value;
}

function configureInput(
  input: HTMLInputElement | undefined,
  role: string,
  placeholder: string,
  label: string,
) {
  if (!input) return;
  input.dataset.role = role;
  if (input.placeholder !== placeholder) input.placeholder = placeholder;
  input.setAttribute("aria-label", label);
  input.title = label;
}

function syncHeader() {
  const subtitle = document.querySelector<HTMLElement>("header > div > span");
  if (subtitle) {
    const id = subtitle.textContent?.split("·").slice(1).join("·").trim();
    const next = id ? `网页控制端 · ${id}` : "网页控制端";
    setText(subtitle, next);
  }

  const status = document.querySelector<HTMLElement>(STATUS_SELECTOR);
  if (!status) return;
  const raw = status.dataset.rawStatus || status.textContent || "idle";
  // React writes the raw status back on every state change. Detect that write,
  // remember it, and then render the Chinese product-facing label.
  const displayed = status.textContent || "";
  if (displayed !== friendlyStatus(raw)) {
    status.dataset.rawStatus = displayed;
  }
  const source = status.dataset.rawStatus || displayed;
  const friendly = friendlyStatus(source);
  setText(status, friendly);
  status.dataset.state = friendly === "已连接" ? "ready" : (friendly === "未连接" ? "idle" : "busy");
}

function syncConnectionCard() {
  const card = document.querySelector<HTMLElement>(CARD_SELECTOR);
  if (!card) return;

  const inputs = Array.from(card.querySelectorAll<HTMLInputElement>("input"));
  const runtimeAuth = inputs.length >= 4;
  card.dataset.authMode = runtimeAuth ? "scoped" : "simple";

  if (runtimeAuth) {
    configureInput(inputs[0], "controller-account", "控制端账号", "控制端账号");
    configureInput(inputs[1], "controller-key", "控制端密钥", "控制端密钥");
    configureInput(inputs[2], "target-device", "远程设备 ID", "远程设备 ID");
    configureInput(inputs[3], "access-code", "访问码", "远程设备访问码");
  } else {
    configureInput(inputs[0], "target-device", "远程设备 ID", "远程设备 ID");
    configureInput(inputs[1], "access-code", "访问码", "远程设备访问码");
  }

  const primary = Array.from(card.querySelectorAll<HTMLButtonElement>("button"))
    .find((button) => !button.closest(".recent-devices"));
  if (primary) {
    const status = document.querySelector<HTMLElement>(STATUS_SELECTOR)?.dataset.rawStatus
      ?? document.querySelector<HTMLElement>(STATUS_SELECTOR)?.textContent
      ?? "idle";
    const idle = status.trim().toLowerCase() === "idle" || status === "未连接";
    setText(primary, idle ? "连接设备" : "断开连接");
    primary.classList.toggle("is-disconnect", !idle);
  }

  const empty = document.querySelector<HTMLElement>(".stage .empty");
  if (empty) {
    setText(
      empty,
      runtimeAuth
        ? "输入控制端凭证、远程设备 ID 和访问码，DeskLink 将优先尝试 Direct P2P 连接。"
        : "输入远程设备 ID 和访问码开始连接。DeskLink 会优先直连，必要时自动切换 TURN 中继。",
    );
  }
}

function sync() {
  syncHeader();
  syncConnectionCard();
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

  new MutationObserver(schedule).observe(root, {
    subtree: true,
    childList: true,
    characterData: true,
    attributes: true,
    attributeFilter: ["disabled"],
  });
  sync();
}

export {};
