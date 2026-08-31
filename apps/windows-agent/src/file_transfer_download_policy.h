#pragma once

#include <windows.h>

namespace desklink {

constexpr DWORD kDownloadFileShareMode = FILE_SHARE_READ;

constexpr const char* DownloadOpenErrorReason(DWORD error) noexcept {
  return error == ERROR_SHARING_VIOLATION ? "file-busy" : "open-file-failed";
}

static_assert((kDownloadFileShareMode & FILE_SHARE_READ) != 0);
static_assert((kDownloadFileShareMode & FILE_SHARE_WRITE) == 0);
static_assert((kDownloadFileShareMode & FILE_SHARE_DELETE) == 0);
static_assert(DownloadOpenErrorReason(ERROR_SHARING_VIOLATION)[0] == 'f');

}  // namespace desklink
