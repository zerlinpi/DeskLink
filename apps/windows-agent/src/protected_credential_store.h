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

// Stores the unattended remote-control access code separately from the device
// bootstrap credential. It uses its own DPAPI entropy and protected file so the
// two secrets can be rotated/removed independently.
bool StoreProtectedAccessCode(
    const std::wstring& access_code,
    std::wstring* error);

ProtectedCredentialStatus LoadProtectedAccessCode(
    std::wstring* access_code,
    std::wstring* error);

bool DeleteProtectedAccessCode(std::wstring* error);

}  // namespace desklink
