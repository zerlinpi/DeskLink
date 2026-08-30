#include "device_identity.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace desklink {
namespace {

constexpr char kDomain[] = "DeskLink stable device id v1\n";

void SetError(std::wstring* error, const std::wstring& message, DWORD code = ERROR_SUCCESS) {
  if (!error) return;
  *error = message;
  if (code != ERROR_SUCCESS) {
    *error += L" (Win32 ";
    *error += std::to_wstring(code);
    *error += L")";
  }
}

std::wstring TrimAndNormalizeGuid(std::wstring_view input) {
  size_t begin = 0;
  while (begin < input.size() && iswspace(input[begin])) ++begin;
  size_t end = input.size();
  while (end > begin && iswspace(input[end - 1])) --end;

  if (end > begin + 1 && input[begin] == L'{' && input[end - 1] == L'}') {
    ++begin;
    --end;
  }

  std::wstring result(input.substr(begin, end - begin));
  std::transform(result.begin(), result.end(), result.begin(), [](wchar_t ch) {
    if (ch >= L'A' && ch <= L'F') return static_cast<wchar_t>(ch - L'A' + L'a');
    return ch;
  });
  return result;
}

bool LooksLikeMachineGuid(const std::wstring& guid) {
  if (guid.size() != 36) return false;
  for (size_t i = 0; i < guid.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (guid[i] != L'-') return false;
      continue;
    }
    const wchar_t ch = guid[i];
    if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f'))) return false;
  }
  return true;
}

std::string WideAscii(const std::wstring& value) {
  std::string result;
  result.reserve(value.size());
  for (wchar_t ch : value) {
    if (ch > 0x7F) return {};
    result.push_back(static_cast<char>(ch));
  }
  return result;
}

bool Sha256(const std::vector<uint8_t>& input, std::array<uint8_t, 32>* digest, std::wstring* error) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::vector<uint8_t> object;

  NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  if (status < 0) {
    SetError(error, L"Unable to open SHA-256 provider");
    return false;
  }

  DWORD object_length = 0;
  DWORD bytes = 0;
  status = BCryptGetProperty(
      algorithm,
      BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<PUCHAR>(&object_length),
      sizeof(object_length),
      &bytes,
      0);
  if (status < 0 || object_length == 0) {
    BCryptCloseAlgorithmProvider(algorithm, 0);
    SetError(error, L"Unable to query SHA-256 object length");
    return false;
  }

  object.resize(object_length);
  status = BCryptCreateHash(
      algorithm,
      &hash,
      object.data(),
      static_cast<ULONG>(object.size()),
      nullptr,
      0,
      0);
  if (status >= 0) {
    status = BCryptHashData(
        hash,
        const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(input.data())),
        static_cast<ULONG>(input.size()),
        0);
  }
  if (status >= 0) {
    status = BCryptFinishHash(hash, digest->data(), static_cast<ULONG>(digest->size()), 0);
  }

  if (hash) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);

  if (status < 0) {
    digest->fill(0);
    SetError(error, L"Unable to derive SHA-256 device identity");
    return false;
  }
  return true;
}

std::wstring ReadMachineGuid(std::wstring* error) {
  HKEY key = nullptr;
  LONG status = RegOpenKeyExW(
      HKEY_LOCAL_MACHINE,
      L"SOFTWARE\\Microsoft\\Cryptography",
      0,
      KEY_QUERY_VALUE | KEY_WOW64_64KEY,
      &key);
  if (status != ERROR_SUCCESS) {
    SetError(error, L"Unable to open Windows MachineGuid registry key", static_cast<DWORD>(status));
    return {};
  }

  DWORD type = 0;
  DWORD bytes = 0;
  status = RegQueryValueExW(key, L"MachineGuid", nullptr, &type, nullptr, &bytes);
  if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
    RegCloseKey(key);
    SetError(
        error,
        L"Windows MachineGuid is unavailable or invalid",
        status == ERROR_SUCCESS ? ERROR_INVALID_DATA : static_cast<DWORD>(status));
    return {};
  }

  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
  status = RegQueryValueExW(
      key,
      L"MachineGuid",
      nullptr,
      &type,
      reinterpret_cast<LPBYTE>(buffer.data()),
      &bytes);
  RegCloseKey(key);
  if (status != ERROR_SUCCESS) {
    SetError(error, L"Unable to read Windows MachineGuid", static_cast<DWORD>(status));
    return {};
  }
  return std::wstring(buffer.data());
}

}  // namespace

std::wstring DeriveStableDeviceId(std::wstring_view machine_guid, std::wstring* error) {
  const std::wstring normalized = TrimAndNormalizeGuid(machine_guid);
  if (!LooksLikeMachineGuid(normalized)) {
    SetError(error, L"Windows MachineGuid has an unexpected format", ERROR_INVALID_DATA);
    return {};
  }

  const std::string ascii_guid = WideAscii(normalized);
  if (ascii_guid.empty()) {
    SetError(error, L"Windows MachineGuid contains unsupported characters", ERROR_INVALID_DATA);
    return {};
  }

  std::vector<uint8_t> material;
  material.reserve(sizeof(kDomain) - 1 + ascii_guid.size());
  material.insert(material.end(), kDomain, kDomain + sizeof(kDomain) - 1);
  material.insert(material.end(), ascii_guid.begin(), ascii_guid.end());

  std::array<uint8_t, 32> digest{};
  if (!Sha256(material, &digest, error)) return {};

  std::wostringstream id;
  id << L"win-" << std::hex << std::setfill(L'0');
  for (size_t i = 0; i < 16; ++i) {
    id << std::setw(2) << static_cast<unsigned int>(digest[i]);
  }
  return id.str();
}

std::wstring StableDefaultDeviceId(std::wstring* error) {
  const std::wstring machine_guid = ReadMachineGuid(error);
  if (machine_guid.empty()) return {};
  return DeriveStableDeviceId(machine_guid, error);
}

}  // namespace desklink
