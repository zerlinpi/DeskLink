import {
  isWakeLockRequestCurrent,
  shouldHoldWakeLock,
} from "./session_wakelock_policy";

let wakeLock: WakeLockSentinel | null = null;
let requesting = false;
let requestGeneration = 0;

function currentStatus() {
  return document.querySelector<HTMLElement>(".status")?.textContent?.trim().toLowerCase() ?? "idle";
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
  const status = currentStatus();
  if (!shouldHoldWakeLock(status, document.visibilityState)) {
    requestGeneration += 1;
    await releaseWakeLock();
    return;
  }
  if (wakeLock || requesting || !("wakeLock" in navigator)) return;

  const generation = ++requestGeneration;
  requesting = true;
  let resyncAfterStaleRequest = false;

  try {
    const sentinel = await navigator.wakeLock.request("screen");
    if (!isWakeLockRequestCurrent(
      generation,
      requestGeneration,
      currentStatus(),
      document.visibilityState,
    )) {
      resyncAfterStaleRequest = shouldHoldWakeLock(currentStatus(), document.visibilityState);
      try {
        await sentinel.release();
      } catch {
      }
      return;
    }

    wakeLock = sentinel;
    sentinel.addEventListener("release", () => {
      if (wakeLock !== sentinel) return;
      wakeLock = null;
      void syncWakeLock();
    }, { once: true });
  } catch (error) {
    console.debug("DeskLink screen wake lock unavailable", error);
  } finally {
    requesting = false;
    if (resyncAfterStaleRequest && !wakeLock) {
      queueMicrotask(() => void syncWakeLock());
    }
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
  window.addEventListener("pagehide", () => {
    requestGeneration += 1;
    void releaseWakeLock();
  });
  void syncWakeLock();
}
