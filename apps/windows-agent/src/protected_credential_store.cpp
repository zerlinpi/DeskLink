#include "protected_credential_store.h"

#include <windows.h>
#include <sddl.h>
#include <wincrypt.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace desklink {
namespace {

constexpr wchar_t kCredentialDirectoryName[] = L"DeskLink";
constexpr wchar_t kCredentialFileName[] = L"device-credential.dpapi";
constexpr wchar_t kRestrictedDacl[] = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)";
constexpr char kEntropy[] = "DeskLink protected device credential v1";
constexpr LONGLONG kMaxProtectedBlobBytes = 64 * 1024;

void SetError(std::wstring* error, const wchar_t* message, DWORD code = ERROR_SUCCESS) {
  if (!error) return;
  *error = message;
  if (code != ERROR_SUCCESS) {
    *error += L" (Win32 ";
    *error += std::to_wstring(code);
    *error += L")";
  }
}

std::wstring ProgramDataPath() {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetEnvironmentVariableW(
      L"ProgramData",
      buffer.data(),
      static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return L"C:\\ProgramData";
  return std::wstring(buffer.data(), length);
}

std::wstring CredentialDirectoryPath() {
  return ProgramDataPath() + L"\\" + kCredentialDirectoryName;
}

std::wstring CredentialFilePath() {
  return CredentialDirectoryPath() + L"\\" + kCredentialFileName;
}

bool ValidDeviceCredential(const std::wstring& credential) {
  if (credential.size() < 8 || credential.size() > 512 || credential.rfind(L"dc1.", 0) != 0) {
    return false;
  }
  return std::all_of(credential.begin(), credential.end(), [](wchar_t ch) {
    return std::iswalnum(ch) || ch == L'.' || ch == L'-' || ch == L'_';
  });
}

bool CreateRestrictedDescriptor(
    PSECURITY_DESCRIPTOR* descriptor,
    std::wstring* error) {
  *descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kRestrictedDacl,
          SDDL_REVISION_1,
          descriptor,
          nullptr)) {
    SetError(error, L"Unable to build protected credential ACL", GetLastError());
    return false;
  }
  return true;
}

bool ApplyRestrictedAcl(
    const std::wstring& path,
    PSECURITY_DESCRIPTOR descriptor,
    std::wstring* error) {
  if (!SetFileSecurityW(
          path.c_str(),
          DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
          descriptor)) {
    SetError(error, L"Unable to apply protected credential ACL", GetLastError());
    return false;
  }
  return true;
}

bool EnsureCredentialDirectory(
    PSECURITY_DESCRIPTOR descriptor,
    std::wstring* error) {
  const std::wstring directory = CredentialDirectoryPath();
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.lpSecurityDescriptor = descriptor;
  attributes.bInheritHandle = FALSE;

  if (!CreateDirectoryW(directory.c_str(), &attributes)) {
    const DWORD create_error = GetLastError();
    if (create_error != ERROR_ALREADY_EXISTS) {
      SetError(error, L"Unable to create protected credential directory", create_error);
      return false;
    }
    const DWORD attributes_value = GetFileAttributesW(directory.c_str());
    if (attributes_value == INVALID_FILE_ATTRIBUTES ||
        (attributes_value & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      SetError(error, L"Protected credential path is not a directory");
      return false;
    }
  }

  return ApplyRestrictedAcl(directory, descriptor, error);
}

DATA_BLOB EntropyBlob() {
  DATA_BLOB blob{};
  blob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
  blob.cbData = static_cast<DWORD>(sizeof(kEntropy) - 1);
  return blob;
}

bool ReadProtectedFile(std::vector<BYTE>* bytes, std::wstring* error) {
  HANDLE file = CreateFileW(
      CredentialFilePath().c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    SetError(error, L"Unable to open protected device credential", GetLastError());
    return false;
  }

  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
      size.QuadPart > kMaxProtectedBlobBytes) {
    const DWORD size_error = GetLastError();
    CloseHandle(file);
    SetError(
        error,
        L"Protected device credential has an invalid size",
        size_error == ERROR_SUCCESS ? ERROR_INVALID_DATA : size_error);
    return false;
  }

  bytes->resize(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL ok = ReadFile(
      file,
      bytes->data(),
      static_cast<DWORD>(bytes->size()),
      &read,
      nullptr);
  const DWORD read_error = ok ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);
  if (!ok || read != bytes->size()) {
    bytes->clear();
    SetError(error, L"Unable to read protected device credential", read_error);
    return false;
  }
  return true;
}

}  // namespace

bool StoreProtectedDeviceCredential(
    const std::wstring& credential,
    std::wstring* error) {
  if (!ValidDeviceCredential(credential)) {
    SetError(error, L"Device credential must be a valid dc1 credential");
    return false;
  }

  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!CreateRestrictedDescriptor(&descriptor, error)) return false;
  if (!EnsureCredentialDirectory(descriptor, error)) {
    LocalFree(descriptor);
    return false;
  }

  DATA_BLOB input{};
  input.pbData = reinterpret_cast<BYTE*>(const_cast<wchar_t*>(credential.data()));
  input.cbData = static_cast<DWORD>(credential.size() * sizeof(wchar_t));
  DATA_BLOB entropy = EntropyBlob();
  DATA_BLOB protected_blob{};
  if (!CryptProtectData(
          &input,
          L"DeskLink device credential",
          &entropy,
          nullptr,
          nullptr,
          CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN,
          &protected_blob)) {
    const DWORD protect_error = GetLastError();
    LocalFree(descriptor);
    SetError(error, L"DPAPI could not protect the device credential", protect_error);
    return false;
  }

  const std::wstring final_path = CredentialFilePath();
  const std::wstring temp_path =
      final_path + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
      std::to_wstring(GetTickCount64());

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.lpSecurityDescriptor = descriptor;
  attributes.bInheritHandle = FALSE;

  HANDLE file = CreateFileW(
      temp_path.c_str(),
      GENERIC_WRITE,
      0,
      &attributes,
      CREATE_NEW,
      FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD create_error = GetLastError();
    LocalFree(protected_blob.pbData);
    LocalFree(descriptor);
    SetError(error, L"Unable to create protected credential file", create_error);
    return false;
  }

  DWORD written = 0;
  const BOOL write_ok = WriteFile(
      file,
      protected_blob.pbData,
      protected_blob.cbData,
      &written,
      nullptr);
  const DWORD write_error = write_ok ? ERROR_SUCCESS : GetLastError();
  const BOOL flush_ok = write_ok && written == protected_blob.cbData && FlushFileBuffers(file);
  const DWORD flush_error = flush_ok ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);
  LocalFree(protected_blob.pbData);

  if (!write_ok || written != protected_blob.cbData || !flush_ok) {
    DeleteFileW(temp_path.c_str());
    LocalFree(descriptor);
    SetError(
        error,
        L"Unable to write protected credential file",
        write_error != ERROR_SUCCESS ? write_error : flush_error);
    return false;
  }

  if (!MoveFileExW(
          temp_path.c_str(),
          final_path.c_str(),
          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD move_error = GetLastError();
    DeleteFileW(temp_path.c_str());
    LocalFree(descriptor);
    SetError(error, L"Unable to install protected credential file", move_error);
    return false;
  }

  const bool acl_ok = ApplyRestrictedAcl(final_path, descriptor, error);
  LocalFree(descriptor);
  return acl_ok;
}

ProtectedCredentialStatus LoadProtectedDeviceCredential(
    std::wstring* credential,
    std::wstring* error) {
  if (!credential) {
    SetError(error, L"Credential output is required");
    return ProtectedCredentialStatus::Error;
  }
  credential->clear();

  const DWORD attributes = GetFileAttributesW(CredentialFilePath().c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD attribute_error = GetLastError();
    if (attribute_error == ERROR_FILE_NOT_FOUND || attribute_error == ERROR_PATH_NOT_FOUND) {
      return ProtectedCredentialStatus::Missing;
    }
    SetError(error, L"Unable to inspect protected credential file", attribute_error);
    return ProtectedCredentialStatus::Error;
  }

  std::vector<BYTE> protected_bytes;
  if (!ReadProtectedFile(&protected_bytes, error)) {
    return ProtectedCredentialStatus::Error;
  }

  DATA_BLOB input{};
  input.pbData = protected_bytes.data();
  input.cbData = static_cast<DWORD>(protected_bytes.size());
  DATA_BLOB entropy = EntropyBlob();
  DATA_BLOB clear_blob{};
  if (!CryptUnprotectData(
          &input,
          nullptr,
          &entropy,
          nullptr,
          nullptr,
          CRYPTPROTECT_UI_FORBIDDEN,
          &clear_blob)) {
    SetError(error, L"DPAPI could not unprotect the device credential", GetLastError());
    return ProtectedCredentialStatus::Error;
  }

  bool valid = clear_blob.cbData > 0 &&
               clear_blob.cbData <= 1024 &&
               clear_blob.cbData % sizeof(wchar_t) == 0;
  if (valid) {
    const auto* value = reinterpret_cast<const wchar_t*>(clear_blob.pbData);
    credential->assign(value, clear_blob.cbData / sizeof(wchar_t));
    valid = ValidDeviceCredential(*credential);
  }

  SecureZeroMemory(clear_blob.pbData, clear_blob.cbData);
  LocalFree(clear_blob.pbData);
  if (!valid) {
    credential->clear();
    SetError(error, L"Protected device credential is invalid");
    return ProtectedCredentialStatus::Error;
  }

  return ProtectedCredentialStatus::Loaded;
}

bool DeleteProtectedDeviceCredential(std::wstring* error) {
  if (DeleteFileW(CredentialFilePath().c_str())) return true;
  const DWORD delete_error = GetLastError();
  if (delete_error == ERROR_FILE_NOT_FOUND || delete_error == ERROR_PATH_NOT_FOUND) return true;
  SetError(error, L"Unable to delete protected device credential", delete_error);
  return false;
}

}  // namespace desklink
