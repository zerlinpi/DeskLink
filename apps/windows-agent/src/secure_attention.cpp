#include "secure_attention.h"

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace desklink {
namespace {

using SendSasFn = VOID(WINAPI*)(BOOL);

constexpr wchar_t kPolicyKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
constexpr wchar_t kPolicyValue[] = L"SoftwareSASGeneration";

void SetError(std::wstring* error, const std::wstring& message, DWORD code = ERROR_SUCCESS) {
  if (!error) return;
  *error = message;
  if (code != ERROR_SUCCESS) {
    *error += L" (Win32 ";
    *error += std::to_wstring(code);
    *error += L")";
  }
}

HMODULE LoadSystemSasModule(std::wstring* error) {
  std::vector<wchar_t> system_dir(32768);
  const UINT length = GetSystemDirectoryW(
      system_dir.data(),
      static_cast<UINT>(system_dir.size()));
  if (length == 0 || length >= system_dir.size()) {
    SetError(error, L"Unable to resolve the Windows system directory", GetLastError());
    return nullptr;
  }

  const std::filesystem::path sas_path =
      std::filesystem::path(std::wstring(system_dir.data(), length)) / L"sas.dll";
  HMODULE module = LoadLibraryW(sas_path.c_str());
  if (!module) {
    SetError(error, L"Unable to load the Windows Secure Attention Sequence API", GetLastError());
  }
  return module;
}

SendSasFn ResolveSendSas(HMODULE module, std::wstring* error) {
  if (!module) return nullptr;
  FARPROC entry = GetProcAddress(module, "SendSAS");
  if (!entry) {
    SetError(error, L"Windows sas.dll does not export SendSAS", GetLastError());
    return nullptr;
  }
  return reinterpret_cast<SendSasFn>(entry);
}

SecureAttentionPolicyMode ParsePolicyValue(DWORD value) {
  switch (value) {
    case 0:
      return SecureAttentionPolicyMode::None;
    case 1:
      return SecureAttentionPolicyMode::Services;
    case 2:
      return SecureAttentionPolicyMode::EaseOfAccess;
    case 3:
      return SecureAttentionPolicyMode::ServicesAndEaseOfAccess;
    default:
      return SecureAttentionPolicyMode::Unknown;
  }
}

bool PolicyAllowsServices(SecureAttentionPolicyMode policy) {
  return policy == SecureAttentionPolicyMode::Services ||
         policy == SecureAttentionPolicyMode::ServicesAndEaseOfAccess;
}

}  // namespace

const char* SecureAttentionPolicyCode(SecureAttentionPolicyMode policy) {
  switch (policy) {
    case SecureAttentionPolicyMode::NotConfigured:
      return "not-configured";
    case SecureAttentionPolicyMode::None:
      return "none";
    case SecureAttentionPolicyMode::Services:
      return "services";
    case SecureAttentionPolicyMode::EaseOfAccess:
      return "ease-of-access";
    case SecureAttentionPolicyMode::ServicesAndEaseOfAccess:
      return "services-and-ease-of-access";
    case SecureAttentionPolicyMode::Unknown:
    default:
      return "unknown";
  }
}

bool SecureAttentionSequenceApiAvailable(std::wstring* error) {
  if (error) error->clear();
  HMODULE module = LoadSystemSasModule(error);
  if (!module) return false;
  const bool available = ResolveSendSas(module, error) != nullptr;
  FreeLibrary(module);
  return available;
}

SecureAttentionCapability QuerySecureAttentionCapability(std::wstring* error) {
  if (error) error->clear();

  SecureAttentionCapability capability;
  std::wstring api_error;
  capability.api_available = SecureAttentionSequenceApiAvailable(&api_error);

  HKEY key = nullptr;
  const LSTATUS open_status = RegOpenKeyExW(
      HKEY_LOCAL_MACHINE,
      kPolicyKey,
      0,
      KEY_QUERY_VALUE | KEY_WOW64_64KEY,
      &key);
  if (open_status == ERROR_FILE_NOT_FOUND) {
    capability.policy = SecureAttentionPolicyMode::NotConfigured;
  } else if (open_status != ERROR_SUCCESS) {
    capability.policy = SecureAttentionPolicyMode::Unknown;
    capability.policy_error = static_cast<DWORD>(open_status);
  } else {
    DWORD value = 0;
    DWORD type = 0;
    DWORD size = sizeof(value);
    const LSTATUS query_status = RegQueryValueExW(
        key,
        kPolicyValue,
        nullptr,
        &type,
        reinterpret_cast<BYTE*>(&value),
        &size);
    RegCloseKey(key);

    if (query_status == ERROR_FILE_NOT_FOUND) {
      capability.policy = SecureAttentionPolicyMode::NotConfigured;
    } else if (query_status != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value)) {
      capability.policy = SecureAttentionPolicyMode::Unknown;
      capability.policy_error = query_status == ERROR_SUCCESS
          ? ERROR_DATATYPE_MISMATCH
          : static_cast<DWORD>(query_status);
    } else {
      capability.policy = ParsePolicyValue(value);
      if (capability.policy == SecureAttentionPolicyMode::Unknown) {
        capability.policy_error = ERROR_INVALID_DATA;
      }
    }
  }

  capability.policy_allows_services = PolicyAllowsServices(capability.policy);

  if (error) {
    if (!capability.api_available) {
      *error = api_error.empty()
          ? L"Windows Secure Attention Sequence API is unavailable"
          : api_error;
    } else if (capability.policy_error != ERROR_SUCCESS) {
      SetError(
          error,
          L"Unable to read a valid SoftwareSASGeneration policy",
          capability.policy_error);
    } else if (!capability.policy_allows_services) {
      *error = L"Windows SoftwareSASGeneration policy does not allow Services";
    }
  }
  return capability;
}

bool SendSecureAttentionSequenceForPipeClient(
    HANDLE pipe,
    std::wstring* error) {
  if (error) error->clear();
  if (!pipe || pipe == INVALID_HANDLE_VALUE) {
    SetError(error, L"A connected authenticated Agent pipe is required", ERROR_INVALID_HANDLE);
    return false;
  }

  std::wstring capability_error;
  const SecureAttentionCapability capability = QuerySecureAttentionCapability(&capability_error);
  if (!capability.api_available || !capability.policy_allows_services) {
    if (error) {
      *error = capability_error.empty()
          ? L"Secure Attention Sequence is not available for Services"
          : capability_error;
    }
    return false;
  }

  HMODULE module = LoadSystemSasModule(error);
  if (!module) return false;
  SendSasFn send_sas = ResolveSendSas(module, error);
  if (!send_sas) {
    FreeLibrary(module);
    return false;
  }

  if (!ImpersonateNamedPipeClient(pipe)) {
    const DWORD impersonate_error = GetLastError();
    FreeLibrary(module);
    SetError(
        error,
        L"Unable to impersonate the authenticated Agent for Secure Attention Sequence",
        impersonate_error);
    return false;
  }

  // Microsoft documents that a Service impersonating its calling process can
  // use SendSAS to target the session associated with that impersonated token.
  // TRUE is used because the worker thread is currently impersonating the user.
  send_sas(TRUE);

  const BOOL reverted = RevertToSelf();
  const DWORD revert_error = reverted ? ERROR_SUCCESS : GetLastError();
  if (!reverted) {
    // A Service must never keep processing requests under a stale client token.
    // Clear the thread token as a defensive fallback before returning failure.
    SetThreadToken(nullptr, nullptr);
  }
  FreeLibrary(module);

  if (!reverted) {
    SetError(error, L"Unable to revert Service impersonation after SendSAS", revert_error);
    return false;
  }
  return true;
}

}  // namespace desklink
