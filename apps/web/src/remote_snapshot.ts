function sessionActive() {
  const value = document.querySelector<HTMLElement>(".status")?.textContent?.trim().toLowerCase() ?? "idle";
  if (["new", "connecting", "connected", "disconnected", "checking"].includes(value)) return true;
  return ["reconnecting", "control ready", "host offline", "negotiating"].some((marker) => value.includes(marker));
}

async function captureRemoteFrame(button: HTMLButtonElement) {
  const video = document.querySelector<HTMLVideoElement>(".stage video");
  if (!video || video.videoWidth <= 0 || video.videoHeight <= 0) {
    button.dataset.feedback = "empty";
    button.textContent = "暂无画面";
    window.setTimeout(() => {
      button.textContent = "截图";
      delete button.dataset.feedback;
    }, 1200);
    return;
  }

  const canvas = document.createElement("canvas");
  canvas.width = video.videoWidth;
  canvas.height = video.videoHeight;
  const context = canvas.getContext("2d", { alpha: false });
  if (!context) return;
  context.drawImage(video, 0, 0, canvas.width, canvas.height);

  const blob = await new Promise<Blob | null>((resolve) => canvas.toBlob(resolve, "image/png"));
  if (!blob) return;

  const target = document.querySelector<HTMLInputElement>('input[placeholder="Remote device ID"]')?.value.trim() || "remote";
  const stamp = new Date().toISOString().replace(/[:.]/g, "-");
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = `DeskLink-${target}-${stamp}.png`;
  anchor.style.display = "none";
  document.body.append(anchor);
  anchor.click();
  anchor.remove();
  window.setTimeout(() => URL.revokeObjectURL(url), 1000);

  button.dataset.feedback = "saved";
  button.textContent = "已截图";
  window.setTimeout(() => {
    button.textContent = "截图";
    delete button.dataset.feedback;
  }, 1100);
}

function sync() {
  const actions = document.querySelector<HTMLElement>(".workbench-actions");
  if (!actions) return;
  let button = actions.querySelector<HTMLButtonElement>("[data-workbench-action='snapshot']");
  if (!button) {
    button = document.createElement("button");
    button.type = "button";
    button.className = "workbench-button";
    button.dataset.workbenchAction = "snapshot";
    button.textContent = "截图";
    button.title = "保存当前远程画面到本地 PNG；图片不会上传到 DeskLink 服务器";
    button.addEventListener("click", () => void captureRemoteFrame(button!));

    const fullscreen = actions.querySelector("[data-workbench-action='fullscreen']");
    actions.insertBefore(button, fullscreen ?? null);
  }
  button.hidden = !sessionActive();
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
