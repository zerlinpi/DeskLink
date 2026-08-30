#pragma once

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
