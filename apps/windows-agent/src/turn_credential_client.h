#pragma once

#include <cstdint>
#include <string>

namespace desklink {

struct RuntimeTurnCredentials {
  std::string username;
  std::string password;
  int64_t expires_at{0};
};

// Fetches temporary TURN credentials over HTTPS using the already-provisioned
// DeskLink signaling registration token. Plain HTTP is accepted only for local
// development endpoints on localhost/loopback.
bool FetchRuntimeTurnCredentials(
    const std::string& endpoint,
    const std::string& device_id,
    const std::string& signal_auth_token,
    RuntimeTurnCredentials* credentials,
    std::string* error);

}  // namespace desklink
