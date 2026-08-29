let wakeLock: WakeLockSentinel | null = null;
let requesting = false;

function currentStatus() {
  return document.querySelector<HTMLElement>(".status")?.textContent?.trim().toLowerCase() ?? "idle";
}

function sessionActive() {
  const value = currentStatus();
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

async function releaseWakeLock() {
  const current = wakeLock;
  wakeLock = null;
  if (!current) return;
  try {
    await current.release();
  } catch {
  }
}

async function syncWakeLock() {
  if (!sessionActive() || document.visibilityState !== "visible") {
    await releaseWakeLock();
    return;
  }
  if (wakeLock || requesting || !("wakeLock" in navigator)) return;

  requesting = true;
  try {
    const sentinel = await navigator.wakeLock.request("screen");
    wakeLock = sentinel;
    sentinel.addEventListener("release", () => {
      if (wakeLock === sentinel) wakeLock = null;
    }, { once: true });
  } catch (error) {
    console.debug("DeskLink screen wake lock unavailable", error);
  } finally {
    requesting = false;
  }
}

const root = document.querySelector("#root");
if (root) {
  let queued = false;
  const schedule = () => {
    if (queued) return;
    queued = true;
    requestAnimationFrame(() => {
      queued = false;
      void syncWakeLock();
    });
  };
  new MutationObserver(schedule).observe(root, {
    childList: true,
    subtree: true,
    characterData: true,
  });
  document.addEventListener("visibilitychange", () => void syncWakeLock());
  window.addEventListener("pagehide", () => void releaseWakeLock());
  void syncWakeLock();
}
