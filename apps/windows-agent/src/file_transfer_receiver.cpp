#include "file_transfer_receiver.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace desklink {
namespace {
using nlohmann::json;

constexpr uint64_t kMaxTransferBytes = 20ULL * 1024 * 1024 * 1024;
constexpr size_t kChunkHeaderBytes = 8 + 32;
constexpr size_t kMaxChunkBytes = 32 * 1024;
constexpr uint64_t kProgressIntervalBytes = 1024 * 1024;
constexpr size_t kMaxListedFiles = 200;

bool Utf8ToWide(const std::string& value, std::wstring* output) {
  if (!output) return false;
  output->clear();
  if (value.empty()) return true;
  if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return false;

  const int length = static_cast<int>(value.size());
  const int required = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      value.data(),
      length,
      nullptr,
      0);
  if (required <= 0) return false;

  output->resize(static_cast<size_t>(required));
  return MultiByteToWideChar(
             CP_UTF8,
             MB_ERR_INVALID_CHARS,
             value.data(),
             length,
             output->data(),
             required) == required;
}

bool WideToUtf8(const std::wstring& value, std::string* output) {
  if (!output) return false;
  output->clear();
  if (value.empty()) return true;
  if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return false;

  const int length = static_cast<int>(value.size());
  const int required = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      value.data(),
      length,
      nullptr,
      0,
      nullptr,
      nullptr);
  if (required <= 0) return false;

  output->resize(static_cast<size_t>(required));
  return WideCharToMultiByte(
             CP_UTF8,
             WC_ERR_INVALID_CHARS,
             value.data(),
             length,
             output->data(),
             required,
             nullptr,
             nullptr) == required;
}

std::string EnvString(const wchar_t* name) {
  wchar_t* value = nullptr;
  size_t length = 0;
  if (_wdupenv_s(&value, &length, name) != 0 || !value) {
    if (value) std::free(value);
    return {};
  }

  const std::wstring wide(value);
  std::free(value);
  std::string result;
  if (!WideToUtf8(wide, &result)) return {};
  return result;
}

std::filesystem::path TransferRoot() {
  std::wstring custom;
  const std::string custom_utf8 = EnvString(L"DESKLINK_TRANSFER_DIR");
  if (!custom_utf8.empty() && Utf8ToWide(custom_utf8, &custom)) {
    return std::filesystem::path(custom);
  }

  std::wstring profile;
  const std::string profile_utf8 = EnvString(L"USERPROFILE");
  if (!profile_utf8.empty() && Utf8ToWide(profile_utf8, &profile)) {
    return std::filesystem::path(profile) / L"Downloads" / L"DeskLink";
  }
  return {};
}

bool ValidTransferId(const std::string& id) {
  if (id.size() < 8 || id.size() > 80) return false;
  return std::all_of(id.begin(), id.end(), [](unsigned char ch) {
    return std::isalnum(ch) != 0 || ch == '-' || ch == '_';
  });
}

bool IsReservedDeviceName(std::wstring name) {
  const size_t dot = name.find(L'.');
  if (dot != std::wstring::npos) name.resize(dot);
  std::transform(name.begin(), name.end(), name.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towupper(ch));
  });
  if (name == L"CON" || name == L"PRN" || name == L"AUX" || name == L"NUL") return true;
  if (name.size() == 4 &&
      (name.rfind(L"COM", 0) == 0 || name.rfind(L"LPT", 0) == 0) &&
      name[3] >= L'1' && name[3] <= L'9') {
    return true;
  }
  return false;
}

bool SafeFileName(const std::string& input, std::wstring* safe_name) {
  if (!safe_name || input.empty() || input.size() > 1024) return false;
  std::wstring wide;
  if (!Utf8ToWide(input, &wide) || wide.empty()) return false;

  for (wchar_t& ch : wide) {
    if (ch < 32 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' ||
        ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*') {
      ch = L'_';
    }
  }
  while (!wide.empty() && (wide.back() == L' ' || wide.back() == L'.')) wide.pop_back();
  if (wide.empty() || wide == L"." || wide == L"..") return false;
  if (wide.size() > 180) wide.resize(180);
  if (IsReservedDeviceName(wide)) wide.insert(wide.begin(), L'_');
  *safe_name = std::move(wide);
  return true;
}

bool StrictExistingFileName(const std::string& input, std::wstring* file_name) {
  if (!file_name || input.empty() || input.size() > 1024) return false;
  std::wstring wide;
  if (!Utf8ToWide(input, &wide) || wide.empty() || wide.size() > 180) return false;
  if (wide == L"." || wide == L".." || IsReservedDeviceName(wide)) return false;
  if (wide.back() == L' ' || wide.back() == L'.') return false;
  for (wchar_t ch : wide) {
    if (ch < 32 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' ||
        ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*') {
      return false;
    }
  }
  if (wide.rfind(L".desklink-upload-", 0) == 0) return false;
  *file_name = std::move(wide);
  return true;
}

uint64_t DecodeLittleEndian64(const uint8_t* data) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(data[i]) << (i * 8);
  }
  return value;
}

void EncodeLittleEndian64(uint64_t value, uint8_t* output) {
  for (size_t i = 0; i < 8; ++i) {
    output[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xff);
  }
}

std::filesystem::path UniqueDestination(
    const std::filesystem::path& root,
    const std::wstring& file_name) {
  const std::filesystem::path original(file_name);
  const std::wstring stem = original.stem().wstring();
  const std::wstring extension = original.extension().wstring();

  std::error_code error;
  std::filesystem::path candidate = root / original;
  if (!std::filesystem::exists(candidate, error)) return candidate;

  for (uint32_t suffix = 1; suffix < 10000; ++suffix) {
    candidate = root / (stem + L" (" + std::to_wstring(suffix) + L")" + extension);
    error.clear();
    if (!std::filesystem::exists(candidate, error)) return candidate;
  }
  return {};
}

bool DownloadableAttributes(DWORD attributes) {
  if (attributes == INVALID_FILE_ATTRIBUTES) return false;
  constexpr DWORD disallowed = FILE_ATTRIBUTE_DIRECTORY |
                               FILE_ATTRIBUTE_REPARSE_POINT |
                               FILE_ATTRIBUTE_HIDDEN |
                               FILE_ATTRIBUTE_SYSTEM;
  return (attributes & disallowed) == 0;
}

}  // namespace

struct FileTransferReceiver::Impl {
  struct ActiveUpload {
    std::string id;
    std::wstring file_name;
    std::filesystem::path partial_path;
    uint64_t expected_size{0};
    uint64_t received{0};
    uint64_t next_progress_at{kProgressIntervalBytes};
    HANDLE file{INVALID_HANDLE_VALUE};
  };

  struct ActiveDownload {
    std::string id;
    std::wstring file_name;
    uint64_t size{0};
    uint64_t next_offset{0};
    HANDLE file{INVALID_HANDLE_VALUE};
  };

  explicit Impl(std::shared_ptr<rtc::DataChannel> input_channel)
      : channel(std::move(input_channel)) {
    if (BCryptOpenAlgorithmProvider(
            &sha256,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0) == 0) {
      DWORD written = 0;
      BCryptGetProperty(
          sha256,
          BCRYPT_OBJECT_LENGTH,
          reinterpret_cast<PUCHAR>(&hash_object_length),
          sizeof(hash_object_length),
          &written,
          0);
      BCryptGetProperty(
          sha256,
          BCRYPT_HASH_LENGTH,
          reinterpret_cast<PUCHAR>(&hash_length),
          sizeof(hash_length),
          &written,
          0);
    }
  }

  ~Impl() {
    std::scoped_lock lock(mutex);
    CloseActive(false);
    CloseDownload();
    if (sha256) BCryptCloseAlgorithmProvider(sha256, 0);
  }

  void CloseActive(bool delete_partial) {
    if (active.file != INVALID_HANDLE_VALUE) {
      CloseHandle(active.file);
      active.file = INVALID_HANDLE_VALUE;
    }
    if (delete_partial && !active.partial_path.empty()) {
      std::error_code ignored;
      std::filesystem::remove(active.partial_path, ignored);
    }
    active = {};
  }

  void CloseDownload() {
    if (download.file != INVALID_HANDLE_VALUE) {
      CloseHandle(download.file);
      download.file = INVALID_HANDLE_VALUE;
    }
    download = {};
  }

  void Send(const json& message) {
    if (!channel || !channel->isOpen()) return;
    try {
      channel->send(message.dump());
    } catch (const std::exception& error) {
      std::cerr << "File transfer response send failed: " << error.what() << "\n";
    }
  }

  bool SendBinary(const uint8_t* data, size_t size) {
    if (!channel || !channel->isOpen() || !data || size == 0) return false;
    if (size > channel->maxMessageSize()) return false;
    try {
      channel->send(reinterpret_cast<const rtc::byte*>(data), size);
      return true;
    } catch (const std::exception& error) {
      std::cerr << "File transfer binary send failed: " << error.what() << "\n";
      return false;
    }
  }

  void SendUploadError(const std::string& id, const std::string& reason) {
    Send(json{{"t", "upload-error"}, {"id", id}, {"error", reason}});
  }

  void SendDownloadError(const std::string& id, const std::string& reason) {
    Send(json{{"t", "download-error"}, {"id", id}, {"error", reason}});
  }

  bool HashChunk(const uint8_t* data, size_t size, std::array<uint8_t, 32>* digest) {
    if (!data || !digest || !sha256 || hash_object_length == 0 || hash_length != digest->size() ||
        size > static_cast<size_t>(std::numeric_limits<ULONG>::max())) {
      return false;
    }

    std::vector<UCHAR> object(hash_object_length);
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS status = BCryptCreateHash(
        sha256,
        &hash,
        object.data(),
        static_cast<ULONG>(object.size()),
        nullptr,
        0,
        0);
    if (status == 0) {
      status = BCryptHashData(
          hash,
          const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(data)),
          static_cast<ULONG>(size),
          0);
    }
    if (status == 0) {
      status = BCryptFinishHash(
          hash,
          digest->data(),
          static_cast<ULONG>(digest->size()),
          0);
    }
    if (hash) BCryptDestroyHash(hash);
    SecureZeroMemory(object.data(), object.size());
    return status == 0;
  }

  bool DigestMatches(const uint8_t* supplied, const uint8_t* data, size_t size) {
    std::array<uint8_t, 32> actual{};
    if (!HashChunk(data, size, &actual)) return false;
    uint8_t difference = 0;
    for (size_t i = 0; i < actual.size(); ++i) difference |= actual[i] ^ supplied[i];
    SecureZeroMemory(actual.data(), actual.size());
    return difference == 0;
  }

  void FinalizeActive() {
    if (active.file == INVALID_HANDLE_VALUE) return;
    FlushFileBuffers(active.file);
    CloseHandle(active.file);
    active.file = INVALID_HANDLE_VALUE;

    const std::filesystem::path root = TransferRoot();
    if (root.empty()) {
      SendUploadError(active.id, "transfer-root-unavailable");
      CloseActive(false);
      return;
    }
    const std::filesystem::path destination = UniqueDestination(root, active.file_name);
    if (destination.empty() ||
        !MoveFileExW(
            active.partial_path.c_str(),
            destination.c_str(),
            MOVEFILE_WRITE_THROUGH)) {
      SendUploadError(active.id, "finalize-failed");
      CloseActive(false);
      return;
    }

    const std::string completed_id = active.id;
    const uint64_t completed_size = active.expected_size;
    const std::wstring completed_name = destination.filename().wstring();
    CloseActive(false);

    std::string utf8_name;
    WideToUtf8(completed_name, &utf8_name);
    Send(json{
        {"t", "upload-complete"},
        {"id", completed_id},
        {"name", utf8_name},
        {"size", completed_size},
    });
  }

  void BeginUpload(const json& message) {
    const std::string id = message.value("id", "");
    const std::string name = message.value("name", "");
    const auto size_it = message.find("size");
    if (!ValidTransferId(id) || size_it == message.end() || !size_it->is_number_unsigned()) {
      SendUploadError(id, "invalid-metadata");
      return;
    }
    const uint64_t expected_size = size_it->get<uint64_t>();
    if (expected_size > kMaxTransferBytes) {
      SendUploadError(id, "file-too-large");
      return;
    }

    std::wstring safe_name;
    if (!SafeFileName(name, &safe_name)) {
      SendUploadError(id, "invalid-file-name");
      return;
    }

    const std::filesystem::path root = TransferRoot();
    if (root.empty()) {
      SendUploadError(id, "transfer-root-unavailable");
      return;
    }
    std::error_code fs_error;
    std::filesystem::create_directories(root, fs_error);
    if (fs_error) {
      SendUploadError(id, "transfer-root-create-failed");
      return;
    }

    CloseActive(false);
    std::wstring wide_id;
    if (!Utf8ToWide(id, &wide_id)) {
      SendUploadError(id, "invalid-transfer-id");
      return;
    }
    const std::filesystem::path partial = root / (L".desklink-upload-" + wide_id + L".part");
    HANDLE file = CreateFileW(
        partial.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      SendUploadError(id, "open-partial-failed");
      return;
    }

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart < 0) {
      CloseHandle(file);
      SendUploadError(id, "partial-size-failed");
      return;
    }
    uint64_t offset = static_cast<uint64_t>(file_size.QuadPart);
    if (offset > expected_size) {
      LARGE_INTEGER zero{};
      if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) {
        CloseHandle(file);
        SendUploadError(id, "partial-reset-failed");
        return;
      }
      offset = 0;
    }

    LARGE_INTEGER seek{};
    seek.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, seek, nullptr, FILE_BEGIN)) {
      CloseHandle(file);
      SendUploadError(id, "partial-seek-failed");
      return;
    }

    active.id = id;
    active.file_name = std::move(safe_name);
    active.partial_path = partial;
    active.expected_size = expected_size;
    active.received = offset;
    active.next_progress_at = ((offset / kProgressIntervalBytes) + 1) * kProgressIntervalBytes;
    active.file = file;

    Send(json{
        {"t", "upload-ready"},
        {"id", id},
        {"offset", offset},
        {"size", expected_size},
    });
    if (offset == expected_size) FinalizeActive();
  }

  void CancelUpload(const json& message) {
    const std::string id = message.value("id", "");
    if (active.file == INVALID_HANDLE_VALUE || id != active.id) return;
    CloseActive(true);
    Send(json{{"t", "upload-cancelled"}, {"id", id}});
  }

  void ListDownloads() {
    const std::filesystem::path root = TransferRoot();
    if (root.empty()) {
      Send(json{{"t", "download-list"}, {"files", json::array()}, {"error", "transfer-root-unavailable"}});
      return;
    }

    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
      Send(json{{"t", "download-list"}, {"files", json::array()}, {"error", "transfer-root-create-failed"}});
      return;
    }

    struct Entry {
      std::string name;
      uint64_t size;
    };
    std::vector<Entry> entries;
    for (std::filesystem::directory_iterator it(root, error), end;
         !error && it != end && entries.size() < kMaxListedFiles;
         it.increment(error)) {
      const auto path = it->path();
      const DWORD attributes = GetFileAttributesW(path.c_str());
      if (!DownloadableAttributes(attributes)) continue;

      std::error_code size_error;
      const uintmax_t raw_size = std::filesystem::file_size(path, size_error);
      if (size_error || raw_size > kMaxTransferBytes) continue;

      const std::wstring wide_name = path.filename().wstring();
      std::string utf8_name;
      std::wstring strict_name;
      if (!WideToUtf8(wide_name, &utf8_name) ||
          !StrictExistingFileName(utf8_name, &strict_name) ||
          strict_name != wide_name) {
        continue;
      }
      entries.push_back({std::move(utf8_name), static_cast<uint64_t>(raw_size)});
    }

    std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
      return left.name < right.name;
    });

    json files = json::array();
    for (const Entry& entry : entries) {
      files.push_back(json{{"name", entry.name}, {"size", entry.size}});
    }
    json response = {{"t", "download-list"}, {"files", std::move(files)}};
    if (error) response["error"] = "transfer-root-list-failed";
    Send(response);
  }

  void BeginDownload(const json& message) {
    const std::string id = message.value("id", "");
    const std::string name = message.value("name", "");
    const auto offset_it = message.find("offset");
    if (!ValidTransferId(id) || offset_it == message.end() || !offset_it->is_number_unsigned()) {
      SendDownloadError(id, "invalid-metadata");
      return;
    }

    std::wstring file_name;
    if (!StrictExistingFileName(name, &file_name)) {
      SendDownloadError(id, "invalid-file-name");
      return;
    }

    const std::filesystem::path root = TransferRoot();
    if (root.empty()) {
      SendDownloadError(id, "transfer-root-unavailable");
      return;
    }
    const std::filesystem::path source = root / file_name;
    const DWORD attributes = GetFileAttributesW(source.c_str());
    if (!DownloadableAttributes(attributes)) {
      SendDownloadError(id, "file-unavailable");
      return;
    }

    HANDLE file = CreateFileW(
        source.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      SendDownloadError(id, "open-file-failed");
      return;
    }

    FILE_ATTRIBUTE_TAG_INFO tag_info{};
    if (!GetFileInformationByHandleEx(
            file,
            FileAttributeTagInfo,
            &tag_info,
            sizeof(tag_info)) ||
        !DownloadableAttributes(tag_info.FileAttributes)) {
      CloseHandle(file);
      SendDownloadError(id, "file-unavailable");
      return;
    }

    LARGE_INTEGER raw_size{};
    if (!GetFileSizeEx(file, &raw_size) || raw_size.QuadPart < 0 ||
        static_cast<uint64_t>(raw_size.QuadPart) > kMaxTransferBytes) {
      CloseHandle(file);
      SendDownloadError(id, "file-size-invalid");
      return;
    }
    const uint64_t size = static_cast<uint64_t>(raw_size.QuadPart);
    const uint64_t offset = offset_it->get<uint64_t>();
    if (offset > size) {
      CloseHandle(file);
      SendDownloadError(id, "invalid-offset");
      return;
    }

    LARGE_INTEGER seek{};
    seek.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, seek, nullptr, FILE_BEGIN)) {
      CloseHandle(file);
      SendDownloadError(id, "seek-failed");
      return;
    }

    CloseDownload();
    download.id = id;
    download.file_name = std::move(file_name);
    download.size = size;
    download.next_offset = offset;
    download.file = file;
    Send(json{{"t", "download-ready"}, {"id", id}, {"name", name}, {"offset", offset}, {"size", size}});
    if (offset == size) {
      Send(json{{"t", "download-complete"}, {"id", id}, {"size", size}});
      CloseDownload();
    }
  }

  void ReadDownload(const json& message) {
    const std::string id = message.value("id", "");
    const auto offset_it = message.find("offset");
    const auto length_it = message.find("length");
    if (download.file == INVALID_HANDLE_VALUE || id != download.id ||
        offset_it == message.end() || !offset_it->is_number_unsigned() ||
        length_it == message.end() || !length_it->is_number_unsigned()) {
      SendDownloadError(id, "download-not-ready");
      return;
    }

    const uint64_t offset = offset_it->get<uint64_t>();
    const uint64_t requested_length = length_it->get<uint64_t>();
    if (offset != download.next_offset) {
      Send(json{{"t", "download-ready"}, {"id", download.id}, {"offset", download.next_offset}, {"size", download.size}});
      return;
    }
    if (requested_length == 0 || requested_length > kMaxChunkBytes || offset >= download.size) {
      SendDownloadError(id, "invalid-read-range");
      return;
    }

    const size_t max_message_size = channel ? channel->maxMessageSize() : 0;
    if (max_message_size <= kChunkHeaderBytes) {
      SendDownloadError(id, "channel-message-size-too-small");
      return;
    }
    const size_t transport_payload_cap = max_message_size - kChunkHeaderBytes;
    const uint64_t remaining = download.size - offset;
    const size_t bytes_to_read = static_cast<size_t>(std::min<uint64_t>({
        requested_length,
        remaining,
        static_cast<uint64_t>(kMaxChunkBytes),
        static_cast<uint64_t>(transport_payload_cap),
    }));
    if (bytes_to_read == 0) {
      SendDownloadError(id, "invalid-read-range");
      return;
    }

    std::vector<uint8_t> payload(bytes_to_read);
    DWORD read = 0;
    if (!ReadFile(download.file, payload.data(), static_cast<DWORD>(payload.size()), &read, nullptr) ||
        read != payload.size()) {
      SendDownloadError(id, "read-failed");
      CloseDownload();
      return;
    }

    std::array<uint8_t, 32> digest{};
    if (!HashChunk(payload.data(), payload.size(), &digest)) {
      SendDownloadError(id, "hash-failed");
      CloseDownload();
      return;
    }

    std::vector<uint8_t> frame(kChunkHeaderBytes + payload.size());
    EncodeLittleEndian64(offset, frame.data());
    std::copy(digest.begin(), digest.end(), frame.begin() + 8);
    std::copy(payload.begin(), payload.end(), frame.begin() + kChunkHeaderBytes);
    SecureZeroMemory(digest.data(), digest.size());
    if (!SendBinary(frame.data(), frame.size())) {
      SendDownloadError(id, "send-failed");
      CloseDownload();
      return;
    }

    download.next_offset += payload.size();
    if (download.next_offset == download.size) {
      Send(json{{"t", "download-complete"}, {"id", download.id}, {"size", download.size}});
      CloseDownload();
    }
  }

  void CancelDownload(const json& message) {
    const std::string id = message.value("id", "");
    if (download.file == INVALID_HANDLE_VALUE || id != download.id) return;
    CloseDownload();
    Send(json{{"t", "download-cancelled"}, {"id", id}});
  }

  void HandleText(const std::string& text) {
    if (text.empty() || text.size() > 16 * 1024) return;
    const json message = json::parse(text, nullptr, false);
    if (message.is_discarded() || !message.is_object()) return;
    const std::string type = message.value("t", "");
    if (type == "upload-begin") BeginUpload(message);
    else if (type == "upload-cancel") CancelUpload(message);
    else if (type == "download-list-request") ListDownloads();
    else if (type == "download-begin") BeginDownload(message);
    else if (type == "download-read") ReadDownload(message);
    else if (type == "download-cancel") CancelDownload(message);
  }

  void HandleBinary(const rtc::binary& binary) {
    if (active.file == INVALID_HANDLE_VALUE || binary.size() < kChunkHeaderBytes ||
        binary.size() > kChunkHeaderBytes + kMaxChunkBytes) {
      return;
    }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(binary.data());
    const uint64_t offset = DecodeLittleEndian64(bytes);
    const uint8_t* supplied_digest = bytes + 8;
    const uint8_t* payload = bytes + kChunkHeaderBytes;
    const size_t payload_size = binary.size() - kChunkHeaderBytes;

    if (offset != active.received) {
      Send(json{{"t", "upload-ready"}, {"id", active.id}, {"offset", active.received}, {"size", active.expected_size}});
      return;
    }
    if (payload_size > active.expected_size - active.received) {
      SendUploadError(active.id, "chunk-overflow");
      return;
    }
    if (!DigestMatches(supplied_digest, payload, payload_size)) {
      SendUploadError(active.id, "chunk-hash-mismatch");
      return;
    }

    DWORD written = 0;
    if (payload_size > 0 &&
        (!WriteFile(
             active.file,
             payload,
             static_cast<DWORD>(payload_size),
             &written,
             nullptr) ||
         written != payload_size)) {
      SendUploadError(active.id, "write-failed");
      CloseActive(false);
      return;
    }

    active.received += payload_size;
    if (active.received >= active.next_progress_at && active.received < active.expected_size) {
      Send(json{
          {"t", "upload-progress"},
          {"id", active.id},
          {"received", active.received},
          {"size", active.expected_size},
      });
      active.next_progress_at =
          ((active.received / kProgressIntervalBytes) + 1) * kProgressIntervalBytes;
    }

    if (active.received == active.expected_size) FinalizeActive();
  }

  std::shared_ptr<rtc::DataChannel> channel;
  std::mutex mutex;
  ActiveUpload active;
  ActiveDownload download;
  BCRYPT_ALG_HANDLE sha256{nullptr};
  DWORD hash_object_length{0};
  DWORD hash_length{0};
};

FileTransferReceiver::FileTransferReceiver(std::shared_ptr<rtc::DataChannel> channel)
    : impl_(std::make_unique<Impl>(std::move(channel))) {}

FileTransferReceiver::~FileTransferReceiver() = default;

std::shared_ptr<FileTransferReceiver> FileTransferReceiver::Create(
    const std::shared_ptr<rtc::DataChannel>& channel) {
  if (!channel) return {};
  return std::shared_ptr<FileTransferReceiver>(new FileTransferReceiver(channel));
}

void FileTransferReceiver::Start() {
  if (!impl_ || !impl_->channel) return;
  const std::weak_ptr<FileTransferReceiver> weak = shared_from_this();

  impl_->channel->onOpen([]() {
    std::cout << "file-transfer DataChannel open\n";
  });
  impl_->channel->onClosed([weak]() {
    if (auto self = weak.lock()) {
      std::scoped_lock lock(self->impl_->mutex);
      self->impl_->CloseActive(false);
      self->impl_->CloseDownload();
    }
    std::cout << "file-transfer DataChannel closed\n";
  });
  impl_->channel->onMessage([weak](rtc::message_variant data) {
    auto self = weak.lock();
    if (!self) return;
    std::scoped_lock lock(self->impl_->mutex);
    if (const auto* text = std::get_if<std::string>(&data)) {
      self->impl_->HandleText(*text);
    } else if (const auto* binary = std::get_if<rtc::binary>(&data)) {
      self->impl_->HandleBinary(*binary);
    }
  });
}

}  // namespace desklink
