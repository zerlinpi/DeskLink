#pragma once

#include <cstdint>
#include <string>

namespace desklink {

// Starts a Service-owned named-pipe broker that exchanges the long-lived device
// credential for short-lived signaling tokens. Only the exact Agent PID is
// allowed to receive responses from this broker.
bool StartServiceAuthBroker(
    const std::wstring& pipe_name,
    uint32_t expected_client_pid,
    const std::string& signal_token_endpoint,
    const std::string& device_id,
    std::string device_credential,
    std::wstring* error);

void StopServiceAuthBroker();

}  // namespace desklink
