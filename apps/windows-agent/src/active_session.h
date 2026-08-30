#pragma once

#include <cstdint>
#include <vector>

namespace desklink {

constexpr uint32_t kInvalidInteractiveSession = 0xFFFFFFFFu;

struct InteractiveSessionCandidate {
  uint32_t session_id{kInvalidInteractiveSession};
  bool active{false};
  bool has_user{false};
};

// Prefer the currently active physical-console user. If the console has no
// active user (common on Windows Server / RDP-only hosts), select a real active
// RDP/interactive user session. Multiple non-console candidates are resolved by
// the lowest session id so selection stays deterministic across polling cycles.
uint32_t SelectActiveInteractiveSession(
    uint32_t console_session_id,
    const std::vector<InteractiveSessionCandidate>& candidates);

// Enumerates Windows Terminal Services sessions and returns the interactive
// session in which desklink-agent.exe should run.
uint32_t FindActiveInteractiveSession();

}  // namespace desklink
