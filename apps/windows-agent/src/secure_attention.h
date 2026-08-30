#pragma once

#include <windows.h>

#include <string>

namespace desklink {

enum class SecureAttentionPolicyMode {
  NotConfigured,
  None,
  Services,
  EaseOfAccess,
  ServicesAndEaseOfAccess,
  Unknown,
};

struct SecureAttentionCapability {
  bool api_available{false};
  bool policy_allows_services{false};
  SecureAttentionPolicyMode policy{SecureAttentionPolicyMode::Unknown};
  DWORD policy_error{ERROR_SUCCESS};
};

// Returns a stable protocol/diagnostic name for the effective local policy.
const char* SecureAttentionPolicyCode(SecureAttentionPolicyMode policy);

// Read-only capability query. DeskLink never changes SoftwareSASGeneration.
// A missing/disabled policy intentionally means Services are not allowed.
SecureAttentionCapability QuerySecureAttentionCapability(std::wstring* error);

// Verifies that the supported Windows Secure Attention Sequence API can be
// resolved from the system Sas.dll. This does not change local security policy.
bool SecureAttentionSequenceApiAvailable(std::wstring* error);

// Called only by the LocalSystem DeskLink Service after the named-pipe client
// PID/SID has already been authenticated. The Service temporarily impersonates
// that exact pipe client so Windows targets the SAS at the Agent's user session.
//
// SendSAS itself has no success return value. This helper therefore refuses to
// dispatch unless the current SoftwareSASGeneration policy explicitly permits
// Services. It never modifies that policy.
bool SendSecureAttentionSequenceForPipeClient(
    HANDLE pipe,
    std::wstring* error);

}  // namespace desklink
