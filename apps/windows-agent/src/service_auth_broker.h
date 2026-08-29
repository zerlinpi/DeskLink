#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace desklink {

// Starts a Service-owned named-pipe broker bound to one exact Agent PID and its
// Windows user SID. Signal-token and access-code capabilities are independent:
// - provide all signal fields to enable short-lived Signal Token exchange;
// - provide access_code to enable protected unattended access-code delivery;
// - at least one capability must be enabled.
bool StartServiceAuthBroker(
    const std::wstring& pipe_name,
    uint32_t expected_client_pid,
    const std::string& signal_token_endpoint,
    const std::string& device_id,
    std::string device_credential,
    std::string access_code,
    std::wstring* error);

// Compatibility overload for the already-deployed signal-token-only Service
// launch path while protected access-code wiring is enabled incrementally.
inline bool StartServiceAuthBroker(
    const std::wstring& pipe_name,
    uint32_t expected_client_pid,
    const std::string& signal_token_endpoint,
    const std::string& device_id,
    std::string device_credential,
    std::wstring* error) {
  return StartServiceAuthBroker(
      pipe_name,
      expected_client_pid,
      signal_token_endpoint,
      device_id,
      std::move(device_credential),
      {},
      error);
}

void StopServiceAuthBroker();

}  // namespace desklink
