type VideoProfile = "auto" | "original" | "high" | "clear";

type ProfileOption = {
  id: VideoProfile;
  label: string;
  detail: string;
};

type ControlChannelDetail = {
  channel: RTCDataChannel;
};

const PROFILE_OPTIONS: ProfileOption[] = [
  { id: "auto", label: "自动", detail: "根据延迟、丢包和带宽动态调整" },
  { id: "original", label: "原画", detail: "保持最高分辨率，弱网只保护码率和帧率" },
  { id: "high", label: "高清", detail: "最高约 900p · 45 fps · 8 Mbps" },
  { id: "clear", label: "清晰", detail: "最高约 720p · 30 fps · 4 Mbps" },
];

const profileById = new Map(PROFILE_OPTIONS.map((profile) => [profile.id, profile]));
let controlChannel: RTCDataChannel | null = null;
let currentProfile: VideoProfile = "auto";
let qualityButton: HTMLButtonElement | null = null;
let qualityMenu: HTMLDivElement | null = null;

function profileLabel(profile: VideoProfile) {
  return profileById.get(profile)?.label ?? "自动";
}

function sendCurrentProfile() {
  if (controlChannel?.readyState !== "open") return false;
  controlChannel.send(JSON.stringify({
    t: "video-profile",
    mode: currentProfile,
  }));
  return true;
}

function syncSelection() {
  if (qualityButton) {
    qualityButton.textContent = `画质 · ${profileLabel(currentProfile)}`;
    qualityButton.dataset.profile = currentProfile;
  }
  qualityMenu?.querySelectorAll<HTMLButtonElement>(".quality-option").forEach((button) => {
    const selected = button.dataset.profile === currentProfile;
    button.classList.toggle("is-selected", selected);
    button.setAttribute("aria-checked", selected ? "true" : "false");
  });
}

function closeMenu() {
  if (!qualityMenu) return;
  qualityMenu.hidden = true;
  qualityButton?.setAttribute("aria-expanded", "false");
}

function attachControlChannel(channel: RTCDataChannel) {
  controlChannel = channel;

  channel.addEventListener("open", () => {
    if (controlChannel !== channel) return;
    sendCurrentProfile();
  });

  channel.addEventListener("close", () => {
    if (controlChannel !== channel) return;
    controlChannel = null;
    currentProfile = "auto";
    syncSelection();
    closeMenu();
  });
}

window.addEventListener("desklink:control-channel", (event) => {
  const detail = (event as CustomEvent<ControlChannelDetail>).detail;
  if (detail?.channel) attachControlChannel(detail.channel);
});

function selectProfile(profile: VideoProfile) {
  currentProfile = profile;
  syncSelection();
  sendCurrentProfile();
  closeMenu();
  document.querySelector<HTMLVideoElement>(".stage video")?.focus();
}

function mountQualityControl() {
  const actions = document.querySelector<HTMLElement>(".workbench-actions");
  if (!actions || actions.querySelector(".quality-control")) return;

  const wrapper = document.createElement("div");
  wrapper.className = "quality-control";

  qualityButton = document.createElement("button");
  qualityButton.type = "button";
  qualityButton.className = "workbench-button quality-trigger";
  qualityButton.dataset.workbenchAction = "quality";
  qualityButton.setAttribute("aria-haspopup", "menu");
  qualityButton.setAttribute("aria-expanded", "false");
  qualityButton.title = "切换远程画面的分辨率、帧率和码率档位";

  qualityMenu = document.createElement("div");
  qualityMenu.className = "quality-menu";
  qualityMenu.setAttribute("role", "radiogroup");
  qualityMenu.setAttribute("aria-label", "远程画质");
  qualityMenu.hidden = true;

  for (const profile of PROFILE_OPTIONS) {
    const option = document.createElement("button");
    option.type = "button";
    option.className = "quality-option";
    option.dataset.profile = profile.id;
    option.setAttribute("role", "radio");

    const title = document.createElement("strong");
    title.textContent = profile.label;
    const detail = document.createElement("span");
    detail.textContent = profile.detail;
    option.append(title, detail);

    option.addEventListener("click", (event) => {
      event.stopPropagation();
      selectProfile(profile.id);
    });
    qualityMenu.append(option);
  }

  qualityButton.addEventListener("click", (event) => {
    event.stopPropagation();
    if (!qualityMenu) return;
    const opening = qualityMenu.hidden;
    qualityMenu.hidden = !opening;
    qualityButton?.setAttribute("aria-expanded", opening ? "true" : "false");
  });

  wrapper.append(qualityButton, qualityMenu);
  const networkButton = actions.querySelector('[data-workbench-action="network"]');
  if (networkButton) {
    actions.insertBefore(wrapper, networkButton);
  } else {
    actions.append(wrapper);
  }
  syncSelection();
}

const observer = new MutationObserver(() => mountQualityControl());
observer.observe(document.documentElement, { subtree: true, childList: true });

window.addEventListener("pointerdown", (event) => {
  const target = event.target;
  if (!(target instanceof Node)) return;
  if (qualityMenu && !qualityMenu.hidden && !qualityMenu.parentElement?.contains(target)) {
    closeMenu();
  }
});

window.addEventListener("keydown", (event) => {
  if (event.key === "Escape") closeMenu();
});

mountQualityControl();

export {};
