type Quality = "pending" | "excellent" | "good" | "fair" | "poor" | "relay";

const NETWORK_SELECTOR = ".network-hud";
const BUTTON_SELECTOR = "[data-workbench-action='network']";

function parseMetric(prefix: string): number | null {
  const hud = document.querySelector<HTMLElement>(NETWORK_SELECTOR);
  if (!hud) return null;
  const item = Array.from(hud.querySelectorAll("span"))
    .find((node) => node.textContent?.trim().startsWith(prefix));
  if (!item?.textContent) return null;
  const match = item.textContent.match(/-?\d+(?:\.\d+)?/);
  if (!match) return null;
  const value = Number(match[0]);
  return Number.isFinite(value) ? value : null;
}

function routeText() {
  return document.querySelector<HTMLElement>(`${NETWORK_SELECTOR} .route`)?.textContent?.trim() ?? "";
}

function classify(): { quality: Quality; label: string } {
  const route = routeText();
  if (/turn relay/i.test(route)) return { quality: "relay", label: "中继链路" };

  const rtt = parseMetric("RTT ");
  const loss = parseMetric("Loss ");
  const jitter = parseMetric("Jitter ");
  const fps = parseMetric("Decode ");
  if (rtt == null && loss == null && jitter == null && fps == null) {
    return { quality: "pending", label: "网络检测中" };
  }

  const latency = rtt ?? 0;
  const packetLoss = loss ?? 0;
  const jitterMs = jitter ?? 0;
  const decodeFps = fps ?? 0;

  if (latency <= 55 && packetLoss < 0.7 && jitterMs < 18 && (decodeFps === 0 || decodeFps >= 45)) {
    return { quality: "excellent", label: "网络优秀" };
  }
  if (latency <= 110 && packetLoss < 1.8 && jitterMs < 35 && (decodeFps === 0 || decodeFps >= 30)) {
    return { quality: "good", label: "网络良好" };
  }
  if (latency <= 190 && packetLoss < 4.5 && jitterMs < 65) {
    return { quality: "fair", label: "网络一般" };
  }
  return { quality: "poor", label: "网络较差" };
}

function sync() {
  const button = document.querySelector<HTMLButtonElement>(BUTTON_SELECTOR);
  if (!button) return;
  const { quality, label } = classify();
  if (button.dataset.quality !== quality) button.dataset.quality = quality;
  const base = "查看实时延迟、丢包、抖动、帧率和可用带宽";
  const title = `${label} · ${base}`;
  if (button.title !== title) button.title = title;
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
    childList: true,
    subtree: true,
    characterData: true,
  });
  sync();
}
