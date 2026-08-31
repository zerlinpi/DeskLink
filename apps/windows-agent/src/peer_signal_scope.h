#pragma once

#include <string_view>

namespace desklink {

inline bool PeerSignalScopeCurrent(
    bool peer_current,
    std::string_view current_controller,
    std::string_view expected_controller,
    std::string_view current_session,
    std::string_view expected_session) {
  return peer_current &&
      !expected_controller.empty() &&
      !expected_session.empty() &&
      current_controller == expected_controller &&
      current_session == expected_session;
}

}  // namespace desklink
