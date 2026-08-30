from pathlib import Path


def replace_once(path: str, old: str, new: str, label: str) -> None:
    file_path = Path(path)
    text = file_path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, got {count}")
    file_path.write_text(text.replace(old, new, 1), encoding="utf-8")


def write(path: str, content: str) -> None:
    Path(path).write_text(content, encoding="utf-8")


write(
    "apps/windows-agent/src/video_policy.h",
    '''#pragma once

#include <cstdint>

namespace desklink {

enum class AdaptationMode : uint8_t {
  Desktop = 0,
  Game = 1,
};

struct AdaptationPolicy {
  uint32_t minimum_fps;
  uint32_t resolution_trigger_fps;
  uint32_t severe_fps_streak;
  uint32_t resolution_pressure_streak;
  uint32_t resolution_change_cooldown_seconds;
  uint32_t bitrate_pressure_percent;
  uint32_t severe_bitrate_percent;
  uint32_t moderate_bitrate_percent;
  bool resolution_requires_fps_trigger;
};

// Desktop mode preserves text clarity and resolution for as long as possible.
// Game mode protects motion/input responsiveness by shedding bitrate and
// resolution before allowing frame rate to collapse.
AdaptationPolicy AdaptationPolicyForMode(AdaptationMode mode, uint32_t fps_ceiling);

// Supported adaptive frame-rate ladder. The configured target may be any value
// from 15..144; transitions use these stable tiers and clamp back to the exact
// configured ceiling when it lies between two tiers. DeskLink keeps 60 fps as
// the compatibility default; 90/120/144 fps are opt-in through DESKLINK_FPS.
uint32_t LowerFpsTier(uint32_t current, uint32_t ceiling);
uint32_t RaiseFpsTier(uint32_t current, uint32_t ceiling);

// A high-refresh stream needs a larger starting media budget than 1080p60.
// Operators can still override DESKLINK_BITRATE_BPS; this is only the default.
uint32_t DefaultBitrateForFps(uint32_t fps);

// Desktop Duplication waits should stay below a frame period. A fixed 16 ms wait
// caps responsiveness around 60 Hz even when the encoder is configured higher.
uint32_t CaptureTimeoutMsForFps(uint32_t fps);

// RTP pacing gets headroom above the configured encoder bitrate so pacing does
// not become the hidden bottleneck for 90/120/144 Hz sessions.
uint32_t PacingBitrateForMedia(uint32_t bitrate_bps);
uint32_t PacingIntervalMsForFps(uint32_t fps);

}  // namespace desklink
''',
)

write(
    "apps/windows-agent/src/video_policy.cpp",
    '''#include "video_policy.h"

#include <algorithm>
#include <cstdint>

namespace desklink {

AdaptationPolicy AdaptationPolicyForMode(AdaptationMode mode, uint32_t fps_ceiling) {
  fps_ceiling = std::clamp<uint32_t>(fps_ceiling, 15, 144);
  if (mode == AdaptationMode::Game) {
    return {
        std::min<uint32_t>(fps_ceiling, 45),  // minimum_fps
        std::min<uint32_t>(fps_ceiling, 45),  // resolution_trigger_fps
        6,   // severe_fps_streak: hold FPS longer before stepping down
        2,   // resolution_pressure_streak: shed pixels earlier
        5,   // resolution_change_cooldown_seconds
        75,  // bitrate_pressure_percent
        60,  // severe_bitrate_percent
        78,  // moderate_bitrate_percent
        false,
    };
  }

  // These values intentionally preserve the pre-mode DeskLink behavior.
  return {
      std::min<uint32_t>(fps_ceiling, 15),
      std::min<uint32_t>(fps_ceiling, 24),
      3,
      4,
      8,
      45,
      65,
      82,
      true,
  };
}

uint32_t LowerFpsTier(uint32_t current, uint32_t ceiling) {
  current = std::min(current, ceiling);
  if (current > 120) return std::min<uint32_t>(ceiling, 120);
  if (current > 90) return std::min<uint32_t>(ceiling, 90);
  if (current > 60) return std::min<uint32_t>(ceiling, 60);
  if (current > 45) return std::min<uint32_t>(ceiling, 45);
  if (current > 30) return std::min<uint32_t>(ceiling, 30);
  if (current > 24) return std::min<uint32_t>(ceiling, 24);
  if (current > 15) return std::min<uint32_t>(ceiling, 15);
  return current;
}

uint32_t RaiseFpsTier(uint32_t current, uint32_t ceiling) {
  current = std::min(current, ceiling);
  if (current >= ceiling) return ceiling;
  if (current < 24) return std::min<uint32_t>(ceiling, 24);
  if (current < 30) return std::min<uint32_t>(ceiling, 30);
  if (current < 45) return std::min<uint32_t>(ceiling, 45);
  if (current < 60) return std::min<uint32_t>(ceiling, 60);
  if (current < 90) return std::min<uint32_t>(ceiling, 90);
  if (current < 120) return std::min<uint32_t>(ceiling, 120);
  if (current < 144) return std::min<uint32_t>(ceiling, 144);
  return ceiling;
}

uint32_t DefaultBitrateForFps(uint32_t fps) {
  if (fps > 120) return 30'000'000;
  if (fps > 90) return 24'000'000;
  if (fps > 60) return 18'000'000;
  return 12'000'000;
}

uint32_t CaptureTimeoutMsForFps(uint32_t fps) {
  fps = std::clamp<uint32_t>(fps, 15, 144);
  const uint32_t rounded_up_frame_ms = (1000u + fps - 1u) / fps;
  return std::clamp<uint32_t>(rounded_up_frame_ms, 2, 16);
}

uint32_t PacingBitrateForMedia(uint32_t bitrate_bps) {
  const uint32_t headroom = std::max<uint32_t>(2'000'000, bitrate_bps / 5);
  const uint64_t paced = static_cast<uint64_t>(bitrate_bps) + headroom;
  return static_cast<uint32_t>(std::min<uint64_t>(paced, 60'000'000));
}

uint32_t PacingIntervalMsForFps(uint32_t fps) {
  if (fps >= 120) return 2;
  if (fps >= 90) return 3;
  return 5;
}

}  // namespace desklink
''',
)

write(
    "apps/windows-agent/src/video_policy_smoke.cpp",
    '''#include "video_policy.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "video policy smoke failed: " << message << "\\n";
    std::exit(1);
  }
}

}  // namespace

int main() {
  using desklink::AdaptationMode;
  using desklink::AdaptationPolicyForMode;
  using desklink::CaptureTimeoutMsForFps;
  using desklink::DefaultBitrateForFps;
  using desklink::LowerFpsTier;
  using desklink::PacingBitrateForMedia;
  using desklink::PacingIntervalMsForFps;
  using desklink::RaiseFpsTier;

  std::vector<uint32_t> down;
  uint32_t fps = 144;
  while (fps > 15) {
    fps = LowerFpsTier(fps, 144);
    down.push_back(fps);
  }
  Require(
      down == std::vector<uint32_t>({120, 90, 60, 45, 30, 24, 15}),
      "144 Hz downgrade ladder changed");

  std::vector<uint32_t> up;
  while (fps < 144) {
    fps = RaiseFpsTier(fps, 144);
    up.push_back(fps);
  }
  Require(
      up == std::vector<uint32_t>({24, 30, 45, 60, 90, 120, 144}),
      "144 Hz recovery ladder changed");

  Require(LowerFpsTier(100, 100) == 90, "non-tier ceiling must step down to 90");
  Require(RaiseFpsTier(90, 100) == 100, "non-tier ceiling must recover exactly");
  Require(DefaultBitrateForFps(60) == 12'000'000, "60 fps default bitrate");
  Require(DefaultBitrateForFps(90) == 18'000'000, "90 fps default bitrate");
  Require(DefaultBitrateForFps(120) == 24'000'000, "120 fps default bitrate");
  Require(DefaultBitrateForFps(144) == 30'000'000, "144 fps default bitrate");
  Require(CaptureTimeoutMsForFps(60) == 16, "60 fps capture timeout");
  Require(CaptureTimeoutMsForFps(90) == 12, "90 fps capture timeout");
  Require(CaptureTimeoutMsForFps(120) == 9, "120 fps capture timeout");
  Require(CaptureTimeoutMsForFps(144) == 7, "144 fps capture timeout");
  Require(PacingBitrateForMedia(30'000'000) == 36'000'000, "144 fps pacing headroom");
  Require(PacingIntervalMsForFps(60) == 5, "60 fps pacing interval");
  Require(PacingIntervalMsForFps(90) == 3, "90 fps pacing interval");
  Require(PacingIntervalMsForFps(144) == 2, "144 fps pacing interval");

  const auto desktop = AdaptationPolicyForMode(AdaptationMode::Desktop, 144);
  Require(desktop.minimum_fps == 15, "desktop minimum FPS must preserve existing behavior");
  Require(desktop.resolution_trigger_fps == 24, "desktop resolution trigger FPS");
  Require(desktop.severe_fps_streak == 3, "desktop FPS pressure streak");
  Require(desktop.resolution_pressure_streak == 4, "desktop resolution pressure streak");
  Require(desktop.resolution_change_cooldown_seconds == 8, "desktop resolution cooldown");
  Require(desktop.bitrate_pressure_percent == 45, "desktop bitrate pressure threshold");
  Require(desktop.severe_bitrate_percent == 65, "desktop severe bitrate factor");
  Require(desktop.moderate_bitrate_percent == 82, "desktop moderate bitrate factor");
  Require(desktop.resolution_requires_fps_trigger, "desktop must exhaust FPS before resolution");

  const auto game = AdaptationPolicyForMode(AdaptationMode::Game, 144);
  Require(game.minimum_fps == 45, "game mode must preserve a 45 FPS floor");
  Require(game.severe_fps_streak == 6, "game mode must hold FPS longer under pressure");
  Require(game.resolution_pressure_streak == 2, "game mode must reduce resolution earlier");
  Require(game.resolution_change_cooldown_seconds == 5, "game resolution cooldown");
  Require(game.bitrate_pressure_percent == 75, "game mode must treat bitrate pressure earlier");
  Require(game.severe_bitrate_percent == 60, "game mode severe bitrate factor");
  Require(game.moderate_bitrate_percent == 78, "game mode moderate bitrate factor");
  Require(!game.resolution_requires_fps_trigger, "game mode must not wait for FPS exhaustion");

  const auto capped_game = AdaptationPolicyForMode(AdaptationMode::Game, 30);
  Require(capped_game.minimum_fps == 30, "game FPS floor must respect a lower configured ceiling");

  std::cout << "High-refresh and adaptation video policy smoke passed.\\n";
  return 0;
}
''',
)

replace_once(
    "apps/windows-agent/src/webrtc_session.h",
    '#include "input_injector.h"\n',
    '#include "input_injector.h"\n#include "video_policy.h"\n',
    "webrtc_session video_policy include",
)
replace_once(
    "apps/windows-agent/src/webrtc_session.h",
    '  std::function<void(VideoProfile)> on_video_profile_requested;\n',
    '  std::function<void(VideoProfile)> on_video_profile_requested;\n  std::function<void(AdaptationMode)> on_adaptation_mode_requested;\n',
    "adaptation callback declaration",
)

video_profile_block = '''  if (type == "video-profile") {\n    if (!config_.on_video_profile_requested) return;\n\n    const std::string mode = event.value("mode", "");\n    if (mode == "auto") {\n      config_.on_video_profile_requested(VideoProfile::Auto);\n    } else if (mode == "original") {\n      config_.on_video_profile_requested(VideoProfile::Original);\n    } else if (mode == "high") {\n      config_.on_video_profile_requested(VideoProfile::High);\n    } else if (mode == "clear") {\n      config_.on_video_profile_requested(VideoProfile::Clear);\n    }\n    return;\n  }\n\n'''
adaptation_block = video_profile_block + '''  if (type == "adaptation-mode") {\n    if (!config_.on_adaptation_mode_requested) return;\n\n    const std::string mode = event.value("mode", "");\n    if (mode == "desktop") {\n      config_.on_adaptation_mode_requested(AdaptationMode::Desktop);\n    } else if (mode == "game") {\n      config_.on_adaptation_mode_requested(AdaptationMode::Game);\n    }\n    return;\n  }\n\n'''
replace_once(
    "apps/windows-agent/src/webrtc_session.cpp",
    video_profile_block,
    adaptation_block,
    "adaptation control protocol",
)

replace_once(
    "apps/windows-agent/src/main.cpp",
    '  uint32_t resolution_healthy_streak = 0;\n\n  desklink::SessionConfig session_config;\n',
    '  uint32_t resolution_healthy_streak = 0;\n  desklink::AdaptationMode adaptation_mode = desklink::AdaptationMode::Desktop;\n\n  desklink::SessionConfig session_config;\n',
    "adaptation mode state",
)

profile_to_network = '''    std::cout << "Controller video profile: " << limits.name\n              << ", fps<= " << limits.max_fps\n              << ", bitrate<= " << (limits.max_bitrate_bps / 1'000'000.0)\n              << " Mbps, resolution-tier " << limits.best_resolution_tier\n              << ".." << limits.worst_resolution_tier << "\\n";\n  };\n  session_config.on_network_feedback = [&](const desklink::NetworkFeedback& feedback) {\n'''
profile_to_network_new = '''    std::cout << "Controller video profile: " << limits.name\n              << ", fps<= " << limits.max_fps\n              << ", bitrate<= " << (limits.max_bitrate_bps / 1'000'000.0)\n              << " Mbps, resolution-tier " << limits.best_resolution_tier\n              << ".." << limits.worst_resolution_tier << "\\n";\n  };\n  session_config.on_adaptation_mode_requested = [&](desklink::AdaptationMode mode) {\n    std::scoped_lock lock(adaptation_mutex);\n    if (adaptation_mode == mode) return;\n    adaptation_mode = mode;\n\n    severe_feedback_streak = 0;\n    healthy_feedback_streak = 0;\n    resolution_pressure_streak = 0;\n    resolution_healthy_streak = 0;\n    const auto now = std::chrono::steady_clock::now();\n    last_bitrate_change = now - std::chrono::seconds(10);\n    last_fps_change = now;\n    last_resolution_change = now - std::chrono::seconds(20);\n\n    std::cout << "Controller adaptation mode: "\n              << (mode == desklink::AdaptationMode::Game ? "game" : "desktop")\n              << "\\n";\n  };\n  session_config.on_network_feedback = [&](const desklink::NetworkFeedback& feedback) {\n'''
replace_once(
    "apps/windows-agent/src/main.cpp",
    profile_to_network,
    profile_to_network_new,
    "adaptation mode callback",
)

replace_once(
    "apps/windows-agent/src/main.cpp",
    '    uint32_t next = current;\n\n    const bool capacity_severe',
    '    uint32_t next = current;\n    const desklink::AdaptationPolicy adaptation_policy =\n        desklink::AdaptationPolicyForMode(adaptation_mode, fps_ceiling);\n\n    const bool capacity_severe',
    "adaptation policy lookup",
)
replace_once(
    "apps/windows-agent/src/main.cpp",
    '      next = std::max<uint32_t>(min_bitrate, static_cast<uint32_t>(current * 0.65));\n',
    '      next = std::max<uint32_t>(\n          min_bitrate,\n          static_cast<uint32_t>(static_cast<uint64_t>(current) *\n                                adaptation_policy.severe_bitrate_percent / 100));\n',
    "severe bitrate adaptation",
)
replace_once(
    "apps/windows-agent/src/main.cpp",
    '      next = std::max<uint32_t>(min_bitrate, static_cast<uint32_t>(current * 0.82));\n',
    '      next = std::max<uint32_t>(\n          min_bitrate,\n          static_cast<uint32_t>(static_cast<uint64_t>(current) *\n                                adaptation_policy.moderate_bitrate_percent / 100));\n',
    "moderate bitrate adaptation",
)
replace_once(
    "apps/windows-agent/src/main.cpp",
    '    if (severe_feedback_streak >= 3 &&\n        now - last_fps_change >= std::chrono::seconds(3)) {\n      const uint32_t lower = desklink::LowerFpsTier(current_fps, fps_ceiling);\n',
    '    if (severe_feedback_streak >= adaptation_policy.severe_fps_streak &&\n        now - last_fps_change >= std::chrono::seconds(3)) {\n      const uint32_t lower = std::max<uint32_t>(\n          adaptation_policy.minimum_fps,\n          desklink::LowerFpsTier(current_fps, fps_ceiling));\n',
    "mode-aware FPS pressure",
)
old_resolution = '''    const uint32_t adaptive_fps = requested_fps.load(std::memory_order_relaxed);\n    const uint32_t fps_floor = std::min<uint32_t>(fps_ceiling, 24);\n    const uint32_t bitrate_pressure_floor = std::max<uint32_t>(\n        min_bitrate,\n        static_cast<uint32_t>(static_cast<uint64_t>(bitrate_ceiling) * 45 / 100));\n    const bool resolution_exhausted = adaptive_fps <= fps_floor &&\n                                      next <= bitrate_pressure_floor;\n\n    if (severe && resolution_exhausted) {\n'''
new_resolution = '''    const uint32_t adaptive_fps = requested_fps.load(std::memory_order_relaxed);\n    const uint32_t bitrate_pressure_floor = std::max<uint32_t>(\n        min_bitrate,\n        static_cast<uint32_t>(static_cast<uint64_t>(bitrate_ceiling) *\n                              adaptation_policy.bitrate_pressure_percent / 100));\n    const bool fps_pressure_ready = !adaptation_policy.resolution_requires_fps_trigger ||\n                                    adaptive_fps <= adaptation_policy.resolution_trigger_fps;\n    const bool resolution_under_pressure = fps_pressure_ready &&\n                                           next <= bitrate_pressure_floor;\n\n    if (severe && resolution_under_pressure) {\n'''
replace_once(
    "apps/windows-agent/src/main.cpp",
    old_resolution,
    new_resolution,
    "mode-aware resolution pressure",
)
replace_once(
    "apps/windows-agent/src/main.cpp",
    '    if (resolution_pressure_streak >= 4 &&\n        now - last_resolution_change >= std::chrono::seconds(8) &&\n',
    '    if (resolution_pressure_streak >= adaptation_policy.resolution_pressure_streak &&\n        now - last_resolution_change >=\n            std::chrono::seconds(adaptation_policy.resolution_change_cooldown_seconds) &&\n',
    "mode-aware resolution streak",
)

write(
    "apps/web/src/adaptation_mode.ts",
    '''export type AdaptationMode = "desktop" | "game";\n\nexport const DEFAULT_ADAPTATION_MODE: AdaptationMode = "desktop";\n\nexport function isAdaptationMode(value: unknown): value is AdaptationMode {\n  return value === "desktop" || value === "game";\n}\n\nexport function adaptationModeMessage(mode: AdaptationMode) {\n  return { t: "adaptation-mode" as const, mode };\n}\n''',
)
write(
    "apps/web/src/adaptation_mode.test.ts",
    '''import { describe, expect, it } from "vitest";\nimport {\n  DEFAULT_ADAPTATION_MODE,\n  adaptationModeMessage,\n  isAdaptationMode,\n} from "./adaptation_mode";\n\ndescribe("adaptation mode control protocol", () => {\n  it("defaults to desktop mode for compatibility", () => {\n    expect(DEFAULT_ADAPTATION_MODE).toBe("desktop");\n  });\n\n  it("accepts only supported mode identifiers", () => {\n    expect(isAdaptationMode("desktop")).toBe(true);\n    expect(isAdaptationMode("game")).toBe(true);\n    expect(isAdaptationMode("auto")).toBe(false);\n    expect(isAdaptationMode(1)).toBe(false);\n  });\n\n  it("encodes the reliable control message expected by the Windows host", () => {\n    expect(adaptationModeMessage("game")).toEqual({\n      t: "adaptation-mode",\n      mode: "game",\n    });\n  });\n});\n''',
)
write(
    "apps/web/src/adaptation_modes.ts",
    '''import {\n  DEFAULT_ADAPTATION_MODE,\n  adaptationModeMessage,\n  isAdaptationMode,\n  type AdaptationMode,\n} from "./adaptation_mode";\n\ntype ModeOption = {\n  id: AdaptationMode;\n  label: string;\n  detail: string;\n};\n\ntype ControlChannelDetail = {\n  channel: RTCDataChannel;\n};\n\nconst STORAGE_KEY = "desklink.adaptation-mode";\nconst MODE_OPTIONS: ModeOption[] = [\n  {\n    id: "desktop",\n    label: "桌面",\n    detail: "弱网优先保持文字清晰度与分辨率，再逐级降低帧率",\n  },\n  {\n    id: "game",\n    label: "游戏",\n    detail: "弱网优先降低码率与分辨率，尽量维持 45 FPS 以上响应",\n  },\n];\n\nlet controlChannel: RTCDataChannel | null = null;\nlet currentMode: AdaptationMode = loadMode();\nlet modeButton: HTMLButtonElement | null = null;\nlet modeMenu: HTMLDivElement | null = null;\n\nfunction loadMode(): AdaptationMode {\n  try {\n    const stored = window.localStorage.getItem(STORAGE_KEY);\n    return isAdaptationMode(stored) ? stored : DEFAULT_ADAPTATION_MODE;\n  } catch {\n    return DEFAULT_ADAPTATION_MODE;\n  }\n}\n\nfunction saveMode(mode: AdaptationMode) {\n  try {\n    window.localStorage.setItem(STORAGE_KEY, mode);\n  } catch {\n    // Persistence is optional; private/locked-down browsers may reject storage.\n  }\n}\n\nfunction optionFor(mode: AdaptationMode) {\n  return MODE_OPTIONS.find((option) => option.id === mode) ?? MODE_OPTIONS[0];\n}\n\nfunction sendCurrentMode() {\n  if (controlChannel?.readyState !== "open") return false;\n  controlChannel.send(JSON.stringify(adaptationModeMessage(currentMode)));\n  return true;\n}\n\nfunction syncSelection() {\n  if (modeButton) {\n    modeButton.textContent = `模式 · ${optionFor(currentMode).label}`;\n    modeButton.dataset.mode = currentMode;\n    modeButton.title = optionFor(currentMode).detail;\n  }\n  modeMenu?.querySelectorAll<HTMLButtonElement>(".adaptation-option").forEach((button) => {\n    const selected = button.dataset.mode === currentMode;\n    button.classList.toggle("is-selected", selected);\n    button.setAttribute("aria-checked", selected ? "true" : "false");\n  });\n}\n\nfunction closeMenu() {\n  if (!modeMenu) return;\n  modeMenu.hidden = true;\n  modeButton?.setAttribute("aria-expanded", "false");\n}\n\nfunction attachControlChannel(channel: RTCDataChannel) {\n  controlChannel = channel;\n  channel.addEventListener("open", () => {\n    if (controlChannel !== channel) return;\n    sendCurrentMode();\n  });\n  channel.addEventListener("close", () => {\n    if (controlChannel !== channel) return;\n    controlChannel = null;\n    closeMenu();\n  });\n}\n\nwindow.addEventListener("desklink:control-channel", (event) => {\n  const detail = (event as CustomEvent<ControlChannelDetail>).detail;\n  if (detail?.channel) attachControlChannel(detail.channel);\n});\n\nfunction selectMode(mode: AdaptationMode) {\n  currentMode = mode;\n  saveMode(mode);\n  syncSelection();\n  sendCurrentMode();\n  closeMenu();\n  document.querySelector<HTMLVideoElement>(".stage video")?.focus();\n}\n\nfunction mountModeControl() {\n  const actions = document.querySelector<HTMLElement>(".workbench-actions");\n  if (!actions || actions.querySelector(".adaptation-control")) return;\n\n  const wrapper = document.createElement("div");\n  wrapper.className = "quality-control adaptation-control";\n\n  modeButton = document.createElement("button");\n  modeButton.type = "button";\n  modeButton.className = "workbench-button adaptation-trigger";\n  modeButton.dataset.workbenchAction = "adaptation";\n  modeButton.setAttribute("aria-haspopup", "menu");\n  modeButton.setAttribute("aria-expanded", "false");\n\n  modeMenu = document.createElement("div");\n  modeMenu.className = "quality-menu adaptation-menu";\n  modeMenu.setAttribute("role", "radiogroup");\n  modeMenu.setAttribute("aria-label", "远程使用模式");\n  modeMenu.hidden = true;\n\n  for (const optionData of MODE_OPTIONS) {\n    const option = document.createElement("button");\n    option.type = "button";\n    option.className = "quality-option adaptation-option";\n    option.dataset.mode = optionData.id;\n    option.setAttribute("role", "radio");\n\n    const title = document.createElement("strong");\n    title.textContent = `${optionData.label}模式`;\n    const detail = document.createElement("span");\n    detail.textContent = optionData.detail;\n    option.append(title, detail);\n\n    option.addEventListener("click", (event) => {\n      event.stopPropagation();\n      selectMode(optionData.id);\n    });\n    modeMenu.append(option);\n  }\n\n  modeButton.addEventListener("click", (event) => {\n    event.stopPropagation();\n    if (!modeMenu) return;\n    const opening = modeMenu.hidden;\n    modeMenu.hidden = !opening;\n    modeButton?.setAttribute("aria-expanded", opening ? "true" : "false");\n  });\n\n  wrapper.append(modeButton, modeMenu);\n  const qualityControl = actions.querySelector(".quality-control:not(.adaptation-control)");\n  if (qualityControl) actions.insertBefore(wrapper, qualityControl);\n  else actions.append(wrapper);\n  syncSelection();\n}\n\nconst observer = new MutationObserver(() => mountModeControl());\nobserver.observe(document.documentElement, { subtree: true, childList: true });\n\nwindow.addEventListener("pointerdown", (event) => {\n  const target = event.target;\n  if (!(target instanceof Node)) return;\n  if (modeMenu && !modeMenu.hidden && !modeMenu.parentElement?.contains(target)) closeMenu();\n});\nwindow.addEventListener("keydown", (event) => {\n  if (event.key === "Escape") closeMenu();\n});\n\nmountModeControl();\n\nexport {};\n''',
)

replace_once(
    "apps/web/index.html",
    '    <script type="module" src="/src/quality_profiles.ts"></script>\n',
    '    <script type="module" src="/src/quality_profiles.ts"></script>\n    <script type="module" src="/src/adaptation_modes.ts"></script>\n',
    "adaptation mode web entry",
)

replace_once(
    "README.md",
    '''后续计划进一步拆分为 **桌面模式** 和 **游戏低延迟模式**：桌面模式优先文字清晰度和高分辨率；游戏模式在弱网时优先降低分辨率和码率、尽量维持高 FPS 与输入响应。''',
    '''Web 工作台现在可直接切换 **桌面模式** / **游戏模式**：桌面模式保持既有策略，弱网时优先保护文字清晰度与分辨率；游戏模式更早降低码率和分辨率，并延后 FPS 降级，默认尽量维持不低于 45 FPS（若配置的 FPS 上限低于 45，则以配置上限为准）。模式选择会保存在浏览器本地，并在控制 DataChannel 重建后自动重新下发。''',
    "README adaptation capability",
)
replace_once(
    "README.md",
    '- Web 端部分网络地址仍是 Vite 构建期配置，后续会增加运行时配置文件以便一个镜像适配不同部署环境。\n',
    '',
    "README stale runtime-config limitation",
)
replace_once(
    "README.md",
    '- 桌面模式 / 游戏模式独立自适应策略。\n',
    '- 继续用真实公网、Wi-Fi 抖动和高刷显示器数据调优桌面/游戏双模式阈值。\n',
    "README M4 adaptation roadmap",
)
