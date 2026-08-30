#include "video_policy.h"

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
