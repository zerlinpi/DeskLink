const STATUS_SELECTOR = ".status";
const CARD_SELECTOR = ".connect-card";

type ConnectionGuidance = {
  level: "warning" | "error";
  title: string;
  text: string;
};

function friendlyStatus(value: string) {
  const normalized = value.trim().toLowerCase();
  if (!normalized || normalized === "idle") return "未连接";
  if (normalized.includes("control ready")) return "已连接";
  if (normalized === "signaling") return "正在连接信令";
  if (normalized.includes("authorizing controller")) return "正在验证控制端";
  if (normalized.includes("authorizing host")) return "正在验证远端";
  if (normalized.includes("proving host access")) return "正在验证访问码";
  if (normalized.includes("negotiating")) return "正在建立远程画面";
  if (normalized.includes("establishing control channel")) return "正在建立控制通道";
  if (normalized.includes("recovering control channel")) return "控制通道恢复中";
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

function guidanceForStatus(value: string): ConnectionGuidance | null {
  const normalized = value.trim().toLowerCase();
  if (normalized.includes("access code rejected")) {
    return { level: "error", title: "访问码不正确", text: "确认被控端 DeskLink.exe 中设置的访问码，再重新连接。" };
  }
  if (normalized.includes("host access code not configured")) {
    return { level: "error", title: "远端未设置访问码", text: "在远端电脑打开 DeskLink.exe，设置无人值守访问码并应用配置。" };
  }
  if (normalized.includes("device revoked")) {
    return { level: "error", title: "设备授权已撤销", text: "该设备已被服务端撤销，需要重新生成并配置有效的设备凭证。" };
  }
  if (normalized.includes("controller authorization rejected")) {
    return { level: "error", title: "控制端无权访问该设备", text: "检查控制端账号、密钥以及服务端 AllowedDevices 授权范围。" };
  }
  if (normalized.includes("turn credential error")) {
    return { level: "error", title: "TURN 凭证获取失败", text: "检查 TURN Credential API、HTTPS 证书以及服务端共享密钥配置。" };
  }
  if (normalized.includes("signal error")) {
    return { level: "error", title: "无法连接信令服务", text: "检查 Signal 地址、WSS 证书、反向代理 WebSocket 升级以及服务器防火墙。" };
  }
  if (normalized.includes("host authentication unavailable")) {
    return { level: "error", title: "远端认证暂不可用", text: "远端 Service/Agent 可能没有正常启动；请在远端 DeskLink.exe 中运行连接诊断。" };
  }
  if (normalized.includes("host offline")) {
    return { level: "warning", title: "远端当前离线", text: "确认远端 Windows 已开机、DeskLink Service 正在运行，并且 Signal 服务可访问。" };
  }
  if (normalized.includes("unsupported host authentication challenge")) {
    return { level: "error", title: "控制端与远端版本不兼容", text: "请将 Windows Host 和 Web 控制端升级到同一版本。" };
  }
  if (normalized.includes("failed") || normalized.includes("error")) {
    return { level: "error", title: "连接没有完成", text: "先检查远端 DeskLink.exe 的连接诊断，再确认 Signal / STUN / TURN 配置。" };
  }
  return null;
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

function rawConnectionStatus() {
  const status = document.querySelector<HTMLElement>(STATUS_SELECTOR);
  return status?.dataset.rawStatus ?? status?.textContent ?? "idle";
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

function syncConnectionGuidance(card: HTMLElement) {
  const guidance = guidanceForStatus(rawConnectionStatus());
  let panel = card.querySelector<HTMLDivElement>(".connection-help");
  if (!guidance) {
    panel?.remove();
    return;
  }

  if (!panel) {
    panel = document.createElement("div");
    panel.className = "connection-help";
    panel.setAttribute("role", "status");
    panel.setAttribute("aria-live", "polite");
    card.append(panel);
  }
  panel.dataset.level = guidance.level;
  panel.replaceChildren();

  const title = document.createElement("strong");
  title.textContent = guidance.title;
  const text = document.createElement("span");
  text.textContent = guidance.text;
  panel.append(title, text);
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
    const status = rawConnectionStatus();
    const idle = status.trim().toLowerCase() === "idle" || status === "未连接";
    primary.dataset.connectionAction = idle ? "connect" : "disconnect";
    primary.setAttribute("aria-label", idle ? "连接远程设备" : "断开远程设备");
    setText(primary, idle ? "连接设备" : "断开连接");
    primary.classList.toggle("is-disconnect", !idle);
  }

  syncConnectionGuidance(card);

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
