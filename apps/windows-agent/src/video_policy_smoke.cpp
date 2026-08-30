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

  std::cout << "High-refresh video policy smoke passed.\n";
  return 0;
}
