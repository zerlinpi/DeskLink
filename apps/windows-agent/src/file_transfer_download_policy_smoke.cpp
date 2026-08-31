#include "file_transfer_download_policy.h"

#include <windows.h>

#include <iostream>
#include <string_view>

int main() {
  wchar_t temp_directory[MAX_PATH + 1]{};
  const DWORD directory_length = GetTempPathW(MAX_PATH, temp_directory);
  if (directory_length == 0 || directory_length > MAX_PATH) {
    std::cerr << "GetTempPathW failed.\n";
    return 1;
  }

  wchar_t temp_file[MAX_PATH + 1]{};
  if (GetTempFileNameW(temp_directory, L"DLK", 0, temp_file) == 0) {
    std::cerr << "GetTempFileNameW failed.\n";
    return 1;
  }

  const auto cleanup = [&]() {
    SetFileAttributesW(temp_file, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(temp_file);
  };

  HANDLE stable = CreateFileW(
      temp_file,
      GENERIC_READ,
      desklink::kDownloadFileShareMode,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (stable == INVALID_HANDLE_VALUE) {
    std::cerr << "Unable to open stable download handle.\n";
    cleanup();
    return 1;
  }

  HANDLE reader = CreateFileW(
      temp_file,
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (reader == INVALID_HANDLE_VALUE) {
    std::cerr << "Stable download policy unexpectedly blocked another reader.\n";
    CloseHandle(stable);
    cleanup();
    return 1;
  }
  CloseHandle(reader);

  SetLastError(ERROR_SUCCESS);
  HANDLE writer = CreateFileW(
      temp_file,
      GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  const DWORD writer_error = GetLastError();
  if (writer != INVALID_HANDLE_VALUE) {
    std::cerr << "Stable download policy allowed a concurrent writer.\n";
    CloseHandle(writer);
    CloseHandle(stable);
    cleanup();
    return 1;
  }
  if (writer_error != ERROR_SHARING_VIOLATION) {
    std::cerr << "Concurrent writer was rejected for an unexpected reason.\n";
    CloseHandle(stable);
    cleanup();
    return 1;
  }

  SetLastError(ERROR_SUCCESS);
  const BOOL deleted_while_open = DeleteFileW(temp_file);
  const DWORD delete_error = GetLastError();
  if (deleted_while_open || delete_error != ERROR_SHARING_VIOLATION) {
    std::cerr << "Stable download policy did not block replacement/deletion.\n";
    CloseHandle(stable);
    cleanup();
    return 1;
  }

  if (std::string_view(desklink::DownloadOpenErrorReason(ERROR_SHARING_VIOLATION)) != "file-busy") {
    std::cerr << "Sharing violation must map to file-busy.\n";
    CloseHandle(stable);
    cleanup();
    return 1;
  }
  if (std::string_view(desklink::DownloadOpenErrorReason(ERROR_ACCESS_DENIED)) != "open-file-failed") {
    std::cerr << "Unexpected open errors must preserve the generic failure reason.\n";
    CloseHandle(stable);
    cleanup();
    return 1;
  }

  CloseHandle(stable);
  if (!DeleteFileW(temp_file)) {
    std::cerr << "Temp file remained locked after stable handle close.\n";
    cleanup();
    return 1;
  }

  std::cout << "DeskLink stable download sharing policy smoke passed.\n";
  return 0;
}
