#pragma once

#include <cstdint>
#include <string>

namespace desklink {

struct RuntimeSignalToken {
  std::string token;
  int64_t expires_at{0};
};

// Exchanges a long-lived device credential for a short-lived signaling token.
// Production endpoints must use HTTPS. Plain HTTP is accepted only for localhost
// so local development can run without a certificate.
bool FetchRuntimeSignalToken(
    const std::string& endpoint,
    const std::string& device_id,
    const std::string& device_credential,
    RuntimeSignalToken* signal_token,
    std::string* error);

}  // namespace desklink
