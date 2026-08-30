#pragma once

#include <string>

#include "signal_token_client.h"

namespace desklink {

// A Service auth Pipe can expose independent capabilities. This prevents a host
// that protects only one secret from accidentally rerouting unrelated behavior.
bool ServiceAuthBrokerConfigured();
bool ServiceSignalTokenBrokerConfigured();
bool ServiceAccessCodeBrokerConfigured();
bool ServiceSecureAttentionBrokerConfigured();

struct ServiceSecureAttentionStatus {
  bool broker_configured{false};
  bool api_available{false};
  bool policy_readable{false};
  bool policy_allows_services{false};
  bool available{false};
  std::string policy{"unknown"};
};

// Reads local Windows SAS API/policy capability and combines it with the launch
// capability declared by the LocalSystem Service. This is read-only and does
// not add another privileged Service RPC.
bool FetchServiceSecureAttentionStatus(
    ServiceSecureAttentionStatus* status,
    std::string* error);

// Requests Windows Ctrl+Alt+Del/SAS through the LocalSystem Service. The Service
// validates the exact Agent PID/SID before temporarily impersonating that pipe
// client and calling the supported Windows SendSAS API. error_code receives a
// stable protocol-safe reason such as policy-not-allowed or rate-limited.
bool RequestServiceSecureAttentionSequence(
    std::string* error,
    std::string* error_code = nullptr);

// Requests a fresh short-lived signal token from the LocalSystem DeskLink Service.
// The long-lived device credential never enters this client path.
bool FetchServiceBrokerSignalToken(
    RuntimeSignalToken* signal_token,
    std::string* error);

// Requests the protected unattended access code from the same PID/SID-bound
// local Service broker. It is intended to be read once by the Agent at startup.
bool FetchServiceBrokerAccessCode(
    std::string* access_code,
    std::string* error);

}  // namespace desklink
