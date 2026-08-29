#pragma once

#include <string>

namespace desklink {

enum class ProtectedCredentialStatus {
  Missing,
  Loaded,
  Error,
};

// Stores the long-lived dc1 device credential using machine-scope DPAPI in an
// administrator/SYSTEM-only file under %ProgramData%\DeskLink.
bool StoreProtectedDeviceCredential(
    const std::wstring& credential,
    std::wstring* error);

ProtectedCredentialStatus LoadProtectedDeviceCredential(
    std::wstring* credential,
    std::wstring* error);

bool DeleteProtectedDeviceCredential(std::wstring* error);

}  // namespace desklink
