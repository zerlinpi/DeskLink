#include "device_identity.h"

#include <iostream>
#include <string>

int wmain() {
  std::wstring error;
  const std::wstring expected = L"win-fb3af00b231f8552d1c7b43b800946df";

  const std::wstring first = desklink::DeriveStableDeviceId(
      L"00112233-4455-6677-8899-AABBCCDDEEFF",
      &error);
  if (first != expected) {
    std::wcerr << L"Stable device identity vector mismatch: " << first
               << L" error=" << error << L"\n";
    return 1;
  }

  const std::wstring normalized = desklink::DeriveStableDeviceId(
      L" {00112233-4455-6677-8899-aabbccddeeff} \r\n",
      &error);
  if (normalized != expected) {
    std::wcerr << L"MachineGuid normalization changed device identity\n";
    return 2;
  }

  const std::wstring other = desklink::DeriveStableDeviceId(
      L"10112233-4455-6677-8899-aabbccddeeff",
      &error);
  if (other.empty() || other == expected) {
    std::wcerr << L"Distinct MachineGuid did not produce a distinct Device ID\n";
    return 3;
  }

  if (!desklink::DeriveStableDeviceId(L"not-a-guid", &error).empty()) {
    std::wcerr << L"Invalid MachineGuid was accepted\n";
    return 4;
  }

  std::wcout << L"Stable device identity derivation smoke passed.\n";
  return 0;
}
