#include "clipboard_win32.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace desklink {
namespace {
using namespace std::chrono_literals;

constexpr size_t kMaxClipboardUtf8Bytes = 1 * 1024 * 1024;
constexpr size_t kMaxClipboardWideChars = kMaxClipboardUtf8Bytes;

void SetError(std::string* error, const char* message) {
  if (error) *error = message ? message : "clipboard operation failed";
}

bool OpenClipboardWithRetry() {
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (OpenClipboard(nullptr)) return true;
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

bool Utf8ToWide(const std::string& text, std::wstring* wide) {
  if (!wide) return false;
  wide->clear();
  if (text.empty()) return true;
  if (text.size() > kMaxClipboardUtf8Bytes || text.find('\0') != std::string::npos) return false;
  if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return false;

  const int source_length = static_cast<int>(text.size());
  const int required = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      text.data(),
      source_length,
      nullptr,
      0);
  if (required <= 0 || static_cast<size_t>(required) > kMaxClipboardWideChars) return false;

  wide->resize(static_cast<size_t>(required));
  return MultiByteToWideChar(
             CP_UTF8,
             MB_ERR_INVALID_CHARS,
             text.data(),
             source_length,
             wide->data(),
             required) == required;
}

bool WideToUtf8(const wchar_t* text, size_t length, std::string* utf8) {
  if (!utf8 || (!text && length != 0)) return false;
  utf8->clear();
  if (length == 0) return true;
  if (length > kMaxClipboardWideChars || length > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return false;
  }

  const int source_length = static_cast<int>(length);
  const int required = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      text,
      source_length,
      nullptr,
      0,
      nullptr,
      nullptr);
  if (required < 0 || static_cast<size_t>(required) > kMaxClipboardUtf8Bytes) return false;
  if (required == 0) return true;

  utf8->resize(static_cast<size_t>(required));
  return WideCharToMultiByte(
             CP_UTF8,
             WC_ERR_INVALID_CHARS,
             text,
             source_length,
             utf8->data(),
             required,
             nullptr,
             nullptr) == required;
}

}  // namespace

bool ReadClipboardTextUtf8(std::string* text, std::string* error) {
  if (!text) {
    SetError(error, "clipboard output is required");
    return false;
  }
  text->clear();
  if (error) error->clear();

  if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
    SetError(error, "remote clipboard does not contain text");
    return false;
  }
  if (!OpenClipboardWithRetry()) {
    SetError(error, "remote clipboard is busy");
    return false;
  }

  bool ok = false;
  HANDLE handle = GetClipboardData(CF_UNICODETEXT);
  if (!handle) {
    SetError(error, "unable to read remote clipboard");
    CloseClipboard();
    return false;
  }

  const wchar_t* value = static_cast<const wchar_t*>(GlobalLock(handle));
  if (!value) {
    SetError(error, "unable to lock remote clipboard data");
    CloseClipboard();
    return false;
  }

  const SIZE_T bytes = GlobalSize(handle);
  const size_t capacity = static_cast<size_t>(bytes / sizeof(wchar_t));
  size_t length = 0;
  while (length < capacity && length <= kMaxClipboardWideChars && value[length] != L'\0') {
    ++length;
  }
  if (length > kMaxClipboardWideChars || length >= capacity) {
    SetError(error, "remote clipboard text is too large or invalid");
  } else if (!WideToUtf8(value, length, text)) {
    SetError(error, "remote clipboard text is not valid Unicode");
  } else {
    ok = true;
  }

  GlobalUnlock(handle);
  CloseClipboard();
  return ok;
}

bool WriteClipboardTextUtf8(const std::string& text, std::string* error) {
  if (error) error->clear();
  if (text.size() > kMaxClipboardUtf8Bytes) {
    SetError(error, "clipboard text exceeds 1 MiB limit");
    return false;
  }

  std::wstring wide;
  if (!Utf8ToWide(text, &wide)) {
    SetError(error, "clipboard text is not valid UTF-8");
    return false;
  }
  if (wide.size() > (SIZE_MAX / sizeof(wchar_t)) - 1) {
    SetError(error, "clipboard text is too large");
    return false;
  }

  const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!memory) {
    SetError(error, "unable to allocate clipboard memory");
    return false;
  }

  void* destination = GlobalLock(memory);
  if (!destination) {
    GlobalFree(memory);
    SetError(error, "unable to lock clipboard memory");
    return false;
  }
  if (!wide.empty()) {
    std::memcpy(destination, wide.data(), wide.size() * sizeof(wchar_t));
  }
  static_cast<wchar_t*>(destination)[wide.size()] = L'\0';
  GlobalUnlock(memory);

  if (!OpenClipboardWithRetry()) {
    GlobalFree(memory);
    SetError(error, "remote clipboard is busy");
    return false;
  }
  if (!EmptyClipboard()) {
    CloseClipboard();
    GlobalFree(memory);
    SetError(error, "unable to clear remote clipboard");
    return false;
  }
  if (!SetClipboardData(CF_UNICODETEXT, memory)) {
    CloseClipboard();
    GlobalFree(memory);
    SetError(error, "unable to write remote clipboard");
    return false;
  }

  // Ownership of memory transfers to the system after SetClipboardData succeeds.
  CloseClipboard();
  return true;
}

}  // namespace desklink
