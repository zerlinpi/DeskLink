#include "video_policy.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "video policy smoke failed: " << message << "\n";
    std::exit(1);
  }
}

}  // namespace

int main() {
  using desklink::AdaptationMode;
  using desklink::AdaptationPolicyForMode;
  using desklink::CaptureTimeoutMsForFps;
  using desklink::ClassifyControlRttMs;
  using desklink::ControlLatencyState;
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

  Require(ClassifyControlRttMs(0.0) == ControlLatencyState::Unknown, "missing control RTT");
  Require(ClassifyControlRttMs(119.9) == ControlLatencyState::Healthy, "healthy control RTT");
  Require(ClassifyControlRttMs(120.0) == ControlLatencyState::Elevated, "elevated control RTT lower bound");
  Require(ClassifyControlRttMs(179.9) == ControlLatencyState::Elevated, "elevated control RTT upper bound");
  Require(ClassifyControlRttMs(180.0) == ControlLatencyState::Moderate, "moderate control RTT lower bound");
  Require(ClassifyControlRttMs(299.9) == ControlLatencyState::Moderate, "moderate control RTT upper bound");
  Require(ClassifyControlRttMs(300.0) == ControlLatencyState::Severe, "severe control RTT lower bound");

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

  std::cout << "High-refresh and adaptation video policy smoke passed.\n";
  return 0;
}
