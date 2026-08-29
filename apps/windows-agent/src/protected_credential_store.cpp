#include "protected_credential_store.h"

#include <windows.h>
#include <sddl.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace desklink {
namespace {

constexpr wchar_t kCredentialDirectoryName[] = L"DeskLink";
constexpr wchar_t kRestrictedDacl[] = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)";
constexpr LONGLONG kMaxProtectedBlobBytes = 64 * 1024;

struct SecretSpec {
  const wchar_t* file_name;
  const char* entropy;
  const wchar_t* description;
  const wchar_t* label;
};

constexpr SecretSpec kDeviceCredentialSpec{
    L"device-credential.dpapi",
    "DeskLink protected device credential v1",
    L"DeskLink device credential",
    L"device credential",
};

constexpr SecretSpec kAccessCodeSpec{
    L"access-code.dpapi",
    "DeskLink protected access code v1",
    L"DeskLink access code",
    L"access code",
};

void SetError(std::wstring* error, const std::wstring& message, DWORD code = ERROR_SUCCESS) {
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

std::wstring SecretFilePath(const SecretSpec& spec) {
  return CredentialDirectoryPath() + L"\\" + spec.file_name;
}

bool ValidDeviceCredential(const std::wstring& credential) {
  const bool supported_prefix =
      credential.rfind(L"dc1.", 0) == 0 || credential.rfind(L"dc2.", 0) == 0;
  if (credential.size() < 8 || credential.size() > 512 || !supported_prefix) {
    return false;
  }
  return std::all_of(credential.begin(), credential.end(), [](wchar_t ch) {
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
           (ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L'-' || ch == L'_';
  });
}

bool ValidAccessCode(const std::wstring& access_code) {
  if (access_code.size() < 8 || access_code.size() > 256) return false;
  return std::all_of(access_code.begin(), access_code.end(), [](wchar_t ch) {
    return ch != L'\r' && ch != L'\n' && ch != L'\0' && !std::iswcntrl(ch);
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
    SetError(error, L"Unable to build protected secret ACL", GetLastError());
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
    SetError(error, L"Unable to apply protected secret ACL", GetLastError());
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
      SetError(error, L"Unable to create protected secret directory", create_error);
      return false;
    }
    const DWORD attributes_value = GetFileAttributesW(directory.c_str());
    if (attributes_value == INVALID_FILE_ATTRIBUTES ||
        (attributes_value & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      SetError(error, L"Protected secret path is not a directory");
      return false;
    }
  }

  return ApplyRestrictedAcl(directory, descriptor, error);
}

DATA_BLOB EntropyBlob(const SecretSpec& spec) {
  DATA_BLOB blob{};
  blob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(spec.entropy));
  blob.cbData = static_cast<DWORD>(std::strlen(spec.entropy));
  return blob;
}

bool ReadProtectedFile(
    const SecretSpec& spec,
    std::vector<BYTE>* bytes,
    std::wstring* error) {
  HANDLE file = CreateFileW(
      SecretFilePath(spec).c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    SetError(
        error,
        std::wstring(L"Unable to open protected ") + spec.label,
        GetLastError());
    return false;
  }

  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
      size.QuadPart > kMaxProtectedBlobBytes) {
    const DWORD size_error = GetLastError();
    CloseHandle(file);
    SetError(
        error,
        std::wstring(L"Protected ") + spec.label + L" has an invalid size",
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
    SetError(
        error,
        std::wstring(L"Unable to read protected ") + spec.label,
        read_error);
    return false;
  }
  return true;
}

using SecretValidator = bool (*)(const std::wstring&);

bool StoreProtectedSecret(
    const SecretSpec& spec,
    const std::wstring& value,
    SecretValidator validator,
    const std::wstring& invalid_message,
    std::wstring* error) {
  if (!validator(value)) {
    SetError(error, invalid_message);
    return false;
  }

  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!CreateRestrictedDescriptor(&descriptor, error)) return false;
  if (!EnsureCredentialDirectory(descriptor, error)) {
    LocalFree(descriptor);
    return false;
  }

  DATA_BLOB input{};
  input.pbData = reinterpret_cast<BYTE*>(const_cast<wchar_t*>(value.data()));
  input.cbData = static_cast<DWORD>(value.size() * sizeof(wchar_t));
  DATA_BLOB entropy = EntropyBlob(spec);
  DATA_BLOB protected_blob{};
  if (!CryptProtectData(
          &input,
          spec.description,
          &entropy,
          nullptr,
          nullptr,
          CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN,
          &protected_blob)) {
    const DWORD protect_error = GetLastError();
    LocalFree(descriptor);
    SetError(
        error,
        std::wstring(L"DPAPI could not protect the ") + spec.label,
        protect_error);
    return false;
  }

  const std::wstring final_path = SecretFilePath(spec);
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
    SetError(error, L"Unable to create protected secret file", create_error);
    return false;
  }

  const DWORD protected_size = protected_blob.cbData;
  DWORD written = 0;
  const BOOL write_ok = WriteFile(
      file,
      protected_blob.pbData,
      protected_size,
      &written,
      nullptr);
  const DWORD write_error = write_ok ? ERROR_SUCCESS : GetLastError();
  const BOOL flush_ok = write_ok && written == protected_size && FlushFileBuffers(file);
  const DWORD flush_error = flush_ok ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);
  LocalFree(protected_blob.pbData);

  if (!write_ok || written != protected_size || !flush_ok) {
    DeleteFileW(temp_path.c_str());
    LocalFree(descriptor);
    SetError(
        error,
        L"Unable to write protected secret file",
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
    SetError(error, L"Unable to install protected secret file", move_error);
    return false;
  }

  const bool acl_ok = ApplyRestrictedAcl(final_path, descriptor, error);
  LocalFree(descriptor);
  return acl_ok;
}

ProtectedCredentialStatus LoadProtectedSecret(
    const SecretSpec& spec,
    std::wstring* value,
    SecretValidator validator,
    std::wstring* error) {
  if (!value) {
    SetError(error, L"Protected secret output is required");
    return ProtectedCredentialStatus::Error;
  }
  value->clear();

  const DWORD attributes = GetFileAttributesW(SecretFilePath(spec).c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD attribute_error = GetLastError();
    if (attribute_error == ERROR_FILE_NOT_FOUND || attribute_error == ERROR_PATH_NOT_FOUND) {
      return ProtectedCredentialStatus::Missing;
    }
    SetError(
        error,
        std::wstring(L"Unable to inspect protected ") + spec.label + L" file",
        attribute_error);
    return ProtectedCredentialStatus::Error;
  }

  std::vector<BYTE> protected_bytes;
  if (!ReadProtectedFile(spec, &protected_bytes, error)) {
    return ProtectedCredentialStatus::Error;
  }

  DATA_BLOB input{};
  input.pbData = protected_bytes.data();
  input.cbData = static_cast<DWORD>(protected_bytes.size());
  DATA_BLOB entropy = EntropyBlob(spec);
  DATA_BLOB clear_blob{};
  if (!CryptUnprotectData(
          &input,
          nullptr,
          &entropy,
          nullptr,
          nullptr,
          CRYPTPROTECT_UI_FORBIDDEN,
          &clear_blob)) {
    SetError(
        error,
        std::wstring(L"DPAPI could not unprotect the ") + spec.label,
        GetLastError());
    return ProtectedCredentialStatus::Error;
  }

  bool valid = clear_blob.cbData > 0 &&
               clear_blob.cbData <= 2048 &&
               clear_blob.cbData % sizeof(wchar_t) == 0;
  if (valid) {
    const auto* clear_value = reinterpret_cast<const wchar_t*>(clear_blob.pbData);
    value->assign(clear_value, clear_blob.cbData / sizeof(wchar_t));
    valid = validator(*value);
  }

  SecureZeroMemory(clear_blob.pbData, clear_blob.cbData);
  LocalFree(clear_blob.pbData);
  if (!valid) {
    value->clear();
    SetError(error, std::wstring(L"Protected ") + spec.label + L" is invalid");
    return ProtectedCredentialStatus::Error;
  }

  return ProtectedCredentialStatus::Loaded;
}

bool DeleteProtectedSecret(const SecretSpec& spec, std::wstring* error) {
  if (DeleteFileW(SecretFilePath(spec).c_str())) return true;
  const DWORD delete_error = GetLastError();
  if (delete_error == ERROR_FILE_NOT_FOUND || delete_error == ERROR_PATH_NOT_FOUND) return true;
  SetError(
      error,
      std::wstring(L"Unable to delete protected ") + spec.label,
      delete_error);
  return false;
}

}  // namespace

bool StoreProtectedDeviceCredential(
    const std::wstring& credential,
    std::wstring* error) {
  return StoreProtectedSecret(
      kDeviceCredentialSpec,
      credential,
      ValidDeviceCredential,
      L"Device credential must be a valid dc1 or dc2 credential",
      error);
}

ProtectedCredentialStatus LoadProtectedDeviceCredential(
    std::wstring* credential,
    std::wstring* error) {
  return LoadProtectedSecret(
      kDeviceCredentialSpec,
      credential,
      ValidDeviceCredential,
      error);
}

bool DeleteProtectedDeviceCredential(std::wstring* error) {
  return DeleteProtectedSecret(kDeviceCredentialSpec, error);
}

bool StoreProtectedAccessCode(
    const std::wstring& access_code,
    std::wstring* error) {
  return StoreProtectedSecret(
      kAccessCodeSpec,
      access_code,
      ValidAccessCode,
      L"Access code must contain 8-256 printable characters",
      error);
}

ProtectedCredentialStatus LoadProtectedAccessCode(
    std::wstring* access_code,
    std::wstring* error) {
  return LoadProtectedSecret(
      kAccessCodeSpec,
      access_code,
      ValidAccessCode,
      error);
}

bool DeleteProtectedAccessCode(std::wstring* error) {
  return DeleteProtectedSecret(kAccessCodeSpec, error);
}

}  // namespace desklink
