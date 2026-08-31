import {
  isConnectAction,
  shouldBlockConnectionInteraction,
  shouldBlockEnterConnect,
  type PreflightInteractionContext,
} from "./browser_preflight_policy";

type CapabilityResult = {
  blocking: string[];
  optional: string[];
};

function inspectBrowser(): CapabilityResult {
  const blocking: string[] = [];
  const optional: string[] = [];

  if (!("WebSocket" in window)) blocking.push("WebSocket");
  if (!("RTCPeerConnection" in window) || !("RTCRtpReceiver" in window)) {
    blocking.push("WebRTC");
  } else {
    const capabilities = RTCRtpReceiver.getCapabilities?.("video");
    const hasH264 = capabilities?.codecs.some(
      (codec) => codec.mimeType.toLowerCase() === "video/h264",
    );
    if (!hasH264) blocking.push("H.264 解码");
  }

  if (!globalThis.crypto?.subtle) blocking.push("Web Crypto");
  if (!("requestFullscreen" in HTMLElement.prototype)) optional.push("全屏");
  if (!("wakeLock" in navigator)) optional.push("屏幕常亮");

  return { blocking, optional };
}

const capability = inspectBrowser();

function renderNotice() {
  const card = document.querySelector<HTMLElement>(".connect-card");
  if (!card) return;

  let notice = card.querySelector<HTMLDivElement>(".browser-preflight");
  if (capability.blocking.length === 0 && capability.optional.length === 0) {
    notice?.remove();
    return;
  }

  if (!notice) {
    notice = document.createElement("div");
    notice.className = "browser-preflight";
    card.append(notice);
  }

  if (capability.blocking.length > 0) {
    notice.dataset.level = "error";
    notice.textContent = `当前浏览器缺少 ${capability.blocking.join(" / ")}，无法建立完整远程桌面。建议使用最新版 Chrome 或 Edge。`;
  } else {
    notice.dataset.level = "warning";
    notice.textContent = `当前浏览器不支持 ${capability.optional.join(" / ")}，远控仍可使用，但部分工具会自动降级。`;
  }
}

function interactionContext(target: EventTarget | null): PreflightInteractionContext {
  const element = target instanceof Element ? target : null;
  return {
    blockingCount: capability.blocking.length,
    insideConnectCard: Boolean(element?.closest(".connect-card")),
    insideRecentDevices: Boolean(element?.closest(".recent-devices")),
  };
}

function isConnectButton(button: HTMLButtonElement) {
  return isConnectAction(button.dataset.connectionAction, button.textContent);
}

function findConnectButton(card: HTMLElement | null): HTMLButtonElement | null {
  if (!card) return null;
  for (const button of card.querySelectorAll<HTMLButtonElement>("button")) {
    if (isConnectButton(button)) return button;
  }
  return null;
}

document.addEventListener("click", (event) => {
  const button = (event.target as Element | null)?.closest<HTMLButtonElement>(".connect-card button");
  if (!button || !isConnectButton(button)) return;
  if (!shouldBlockConnectionInteraction(interactionContext(button))) return;
  event.preventDefault();
  event.stopImmediatePropagation();
  renderNotice();
}, true);

document.addEventListener("keydown", (event) => {
  if (event.key !== "Enter") return;
  const element = event.target instanceof Element ? event.target : null;
  const card = element?.closest<HTMLElement>(".connect-card") ?? null;
  const primary = findConnectButton(card);
  if (!shouldBlockEnterConnect(interactionContext(event.target), Boolean(primary))) return;
  event.preventDefault();
  event.stopImmediatePropagation();
  renderNotice();
}, true);

const root = document.querySelector("#root");
if (root) {
  let queued = false;
  const schedule = () => {
    if (queued) return;
    queued = true;
    requestAnimationFrame(() => {
      queued = false;
      renderNotice();
    });
  };
  new MutationObserver(schedule).observe(root, { childList: true, subtree: true });
  renderNotice();
}
