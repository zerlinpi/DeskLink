#pragma once

#include <string>
#include <string_view>

namespace desklink {

// Derives the public DeskLink Device ID from a Windows MachineGuid without
// exposing the MachineGuid itself. The result is deterministic and contains
// 128 bits of a domain-separated SHA-256 digest.
std::wstring DeriveStableDeviceId(std::wstring_view machine_guid, std::wstring* error = nullptr);

// Reads HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid and derives the
// default Device ID. Existing configured DESKLINK_DEVICE_ID values still take
// precedence; this is only the stable default for first-time provisioning.
std::wstring StableDefaultDeviceId(std::wstring* error = nullptr);

}  // namespace desklink
