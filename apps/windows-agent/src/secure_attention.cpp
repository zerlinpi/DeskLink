#include "secure_attention.h"

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace desklink {
namespace {

using SendSasFn = VOID(WINAPI*)(BOOL);

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

}  // namespace

bool SecureAttentionSequenceApiAvailable(std::wstring* error) {
  if (error) error->clear();
  HMODULE module = LoadSystemSasModule(error);
  if (!module) return false;
  const bool available = ResolveSendSas(module, error) != nullptr;
  FreeLibrary(module);
  return available;
}

bool SendSecureAttentionSequenceForPipeClient(
    HANDLE pipe,
    std::wstring* error) {
  if (error) error->clear();
  if (!pipe || pipe == INVALID_HANDLE_VALUE) {
    SetError(error, L"A connected authenticated Agent pipe is required", ERROR_INVALID_HANDLE);
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
