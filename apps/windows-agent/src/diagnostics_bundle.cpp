#include "diagnostics_bundle.h"

#include <windows.h>
#include <dxgi1_2.h>
#include <winsvc.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace desklink {
namespace {

using Microsoft::WRL::ComPtr;
using nlohmann::json;

constexpr uint64_t kMaximumArchiveBytes = 16ULL * 1024ULL * 1024ULL;
constexpr uint16_t kZipUtf8Flag = 0x0800;
constexpr std::array<std::string_view, 7> kSectionNames = {
    "system",
    "desklink",
    "session",
    "network",
    "media",
    "service",
    "logs",
};

struct ZipEntry {
  std::string name;
  std::string data;
  uint32_t crc32 = 0;
  uint32_t local_offset = 0;
};

void SetError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

std::string WideToUtf8(std::wstring_view value) {
  if (value.empty()) return {};
  const int required = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0,
      nullptr,
      nullptr);
  if (required <= 0) return {};

  std::string output(static_cast<size_t>(required), '\0');
  if (WideCharToMultiByte(
          CP_UTF8,
          WC_ERR_INVALID_CHARS,
          value.data(),
          static_cast<int>(value.size()),
          output.data(),
          required,
          nullptr,
          nullptr) != required) {
    return {};
  }
  return output;
}

std::string ReadRegistryString(
    HKEY root,
    const wchar_t* subkey,
    const wchar_t* value_name) {
  DWORD bytes = 0;
  const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
  if (RegGetValueW(root, subkey, value_name, flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
      bytes < sizeof(wchar_t)) {
    return {};
  }

  std::vector<wchar_t> value((bytes / sizeof(wchar_t)) + 1, L'\0');
  if (RegGetValueW(
          root,
          subkey,
          value_name,
          flags,
          nullptr,
          value.data(),
          &bytes) != ERROR_SUCCESS) {
    return {};
  }
  const size_t length = wcsnlen_s(value.data(), value.size());
  return WideToUtf8(std::wstring_view(value.data(), length));
}

std::string UtcTimestamp() {
  SYSTEMTIME time{};
  GetSystemTime(&time);
  char buffer[32]{};
  std::snprintf(
      buffer,
      sizeof(buffer),
      "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
      static_cast<unsigned>(time.wYear),
      static_cast<unsigned>(time.wMonth),
      static_cast<unsigned>(time.wDay),
      static_cast<unsigned>(time.wHour),
      static_cast<unsigned>(time.wMinute),
      static_cast<unsigned>(time.wSecond),
      static_cast<unsigned>(time.wMilliseconds));
  return buffer;
}

std::string NormalizeKey(std::string_view key) {
  std::string normalized;
  normalized.reserve(key.size());
  for (const unsigned char value : key) {
    if (std::isalnum(value)) {
      normalized.push_back(static_cast<char>(std::tolower(value)));
    }
  }
  return normalized;
}

std::string LowerAscii(std::string_view value) {
  std::string lowered;
  lowered.reserve(value.size());
  for (const unsigned char byte : value) {
    lowered.push_back(static_cast<char>(std::tolower(byte)));
  }
  return lowered;
}

bool IsSensitiveKey(std::string_view key) {
  const std::string normalized = NormalizeKey(key);
  static constexpr std::array<std::string_view, 15> kSensitiveKeys = {
      "accesscode",
      "devicecredential",
      "controllertoken",
      "turnsecret",
      "signalsecret",
      "privatekey",
      "dpapiblob",
      "authorization",
      "password",
      "authtoken",
      "bearertoken",
      "accesstoken",
      "refreshtoken",
      "authsecret",
      "credentialsecret",
  };
  return std::find(kSensitiveKeys.begin(), kSensitiveKeys.end(), normalized) !=
         kSensitiveKeys.end();
}

bool ContainsSensitiveLabel(std::string_view text) {
  const std::string lowered = LowerAscii(text);
  static constexpr std::array<std::string_view, 20> kLabels = {
      "access code",
      "access_code",
      "access-code",
      "device credential",
      "device_credential",
      "device-credential",
      "controller token",
      "controller_token",
      "controller-token",
      "turn secret",
      "turn_secret",
      "signal secret",
      "signal_secret",
      "private key",
      "private_key",
      "dpapi blob",
      "dpapi_blob",
      "authorization:",
      "bearer ",
      "-----begin ",
  };
  return std::any_of(kLabels.begin(), kLabels.end(), [&](std::string_view label) {
    return lowered.find(label) != std::string::npos;
  });
}

std::string RedactText(
    std::string value,
    const std::vector<std::string>& known_secret_values) {
  for (const std::string& secret : known_secret_values) {
    if (secret.empty()) continue;
    size_t offset = 0;
    while ((offset = value.find(secret, offset)) != std::string::npos) {
      value.replace(offset, secret.size(), "[REDACTED]");
      offset += std::string_view("[REDACTED]").size();
    }
  }
  if (ContainsSensitiveLabel(value)) return "[REDACTED]";
  return value;
}

json RedactValue(
    const json& value,
    const std::vector<std::string>& known_secret_values) {
  if (value.is_object()) {
    json redacted = json::object();
    for (auto item = value.begin(); item != value.end(); ++item) {
      if (IsSensitiveKey(item.key())) {
        redacted[item.key()] = "[REDACTED]";
      } else {
        redacted[item.key()] = RedactValue(item.value(), known_secret_values);
      }
    }
    return redacted;
  }
  if (value.is_array()) {
    json redacted = json::array();
    for (const auto& item : value) {
      redacted.push_back(RedactValue(item, known_secret_values));
    }
    return redacted;
  }
  if (value.is_string()) {
    return RedactText(value.get<std::string>(), known_secret_values);
  }
  return value;
}

json CollectWindowsInfo() {
  constexpr wchar_t kWindowsKey[] =
      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
  return {
      {"product_name", ReadRegistryString(HKEY_LOCAL_MACHINE, kWindowsKey, L"ProductName")},
      {"display_version", ReadRegistryString(HKEY_LOCAL_MACHINE, kWindowsKey, L"DisplayVersion")},
      {"build_number", ReadRegistryString(HKEY_LOCAL_MACHINE, kWindowsKey, L"CurrentBuildNumber")},
  };
}

json CollectCpuInfo() {
  constexpr wchar_t kCpuKey[] =
      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
  return {
      {"name", ReadRegistryString(HKEY_LOCAL_MACHINE, kCpuKey, L"ProcessorNameString")},
      {"logical_processors", GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)},
  };
}

json CollectMemoryInfo() {
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status)) {
    return {
        {"physical_total_bytes", nullptr},
        {"physical_available_bytes", nullptr},
        {"load_percent", nullptr},
    };
  }
  return {
      {"physical_total_bytes", status.ullTotalPhys},
      {"physical_available_bytes", status.ullAvailPhys},
      {"load_percent", status.dwMemoryLoad},
  };
}

std::string FormatDriverVersion(const LARGE_INTEGER& version) {
  const uint64_t packed = static_cast<uint64_t>(version.QuadPart);
  char buffer[32]{};
  std::snprintf(
      buffer,
      sizeof(buffer),
      "%u.%u.%u.%u",
      static_cast<unsigned>((packed >> 48U) & 0xffffU),
      static_cast<unsigned>((packed >> 32U) & 0xffffU),
      static_cast<unsigned>((packed >> 16U) & 0xffffU),
      static_cast<unsigned>(packed & 0xffffU));
  return buffer;
}

json CollectGpuInfo() {
  json adapters = json::array();
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return adapters;

  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    if (!adapter) continue;

    DXGI_ADAPTER_DESC1 description{};
    if (FAILED(adapter->GetDesc1(&description))) continue;

    json value = {
        {"name", WideToUtf8(description.Description)},
        {"vendor_id", description.VendorId},
        {"device_id", description.DeviceId},
        {"dedicated_video_memory_bytes", description.DedicatedVideoMemory},
        {"shared_system_memory_bytes", description.SharedSystemMemory},
        {"software_adapter", (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0},
        {"driver_version", nullptr},
    };
    LARGE_INTEGER driver_version{};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver_version))) {
      value["driver_version"] = FormatDriverVersion(driver_version);
    }
    adapters.push_back(std::move(value));
  }
  return adapters;
}

json CollectDisplayInfo() {
  json displays = json::array();
  for (DWORD index = 0;; ++index) {
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    if (!EnumDisplayDevicesW(nullptr, index, &device, 0)) break;
    if ((device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0) continue;

    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    const bool has_mode =
        EnumDisplaySettingsExW(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0) != FALSE;
    displays.push_back({
        {"device_name", WideToUtf8(device.DeviceName)},
        {"description", WideToUtf8(device.DeviceString)},
        {"primary", (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0},
        {"width", has_mode ? json(mode.dmPelsWidth) : json(nullptr)},
        {"height", has_mode ? json(mode.dmPelsHeight) : json(nullptr)},
        {"refresh_hz", has_mode ? json(mode.dmDisplayFrequency) : json(nullptr)},
    });
  }
  return displays;
}

std::string ServiceState() {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!manager) return "unavailable";
  SC_HANDLE service = OpenServiceW(manager, L"DeskLink", SERVICE_QUERY_STATUS);
  if (!service) {
    const DWORD open_error = GetLastError();
    CloseServiceHandle(manager);
    return open_error == ERROR_SERVICE_DOES_NOT_EXIST ? "not-installed" : "unavailable";
  }

  SERVICE_STATUS_PROCESS status{};
  DWORD bytes_needed = 0;
  const BOOL queried = QueryServiceStatusEx(
      service,
      SC_STATUS_PROCESS_INFO,
      reinterpret_cast<LPBYTE>(&status),
      sizeof(status),
      &bytes_needed);
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  if (!queried) return "unavailable";

  switch (status.dwCurrentState) {
    case SERVICE_STOPPED:
      return "stopped";
    case SERVICE_START_PENDING:
      return "starting";
    case SERVICE_STOP_PENDING:
      return "stopping";
    case SERVICE_RUNNING:
      return "running";
    case SERVICE_CONTINUE_PENDING:
      return "continue-pending";
    case SERVICE_PAUSE_PENDING:
      return "pause-pending";
    case SERVICE_PAUSED:
      return "paused";
    default:
      return "unknown";
  }
}

uint32_t Crc32(std::string_view data) {
  uint32_t crc = 0xffffffffU;
  for (const unsigned char byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return ~crc;
}

void AppendU16(std::vector<uint8_t>* output, uint16_t value) {
  output->push_back(static_cast<uint8_t>(value & 0xffU));
  output->push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
}

void AppendU32(std::vector<uint8_t>* output, uint32_t value) {
  output->push_back(static_cast<uint8_t>(value & 0xffU));
  output->push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
  output->push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
  output->push_back(static_cast<uint8_t>((value >> 24U) & 0xffU));
}

void AppendText(std::vector<uint8_t>* output, std::string_view value) {
  output->insert(output->end(), value.begin(), value.end());
}

bool BuildStoredZip(
    std::vector<ZipEntry>* entries,
    std::vector<uint8_t>* output,
    std::string* error) {
  output->clear();
  for (ZipEntry& entry : *entries) {
    if (entry.name.size() > std::numeric_limits<uint16_t>::max() ||
        entry.data.size() > std::numeric_limits<uint32_t>::max() ||
        output->size() > std::numeric_limits<uint32_t>::max()) {
      SetError(error, "diagnostics archive entry exceeds ZIP limits");
      return false;
    }
    const uint64_t projected_size =
        static_cast<uint64_t>(output->size()) + 30ULL + entry.name.size() +
        entry.data.size();
    if (projected_size > kMaximumArchiveBytes) {
      SetError(error, "diagnostics archive exceeds the 16 MiB local export limit");
      return false;
    }
    entry.crc32 = Crc32(entry.data);
    entry.local_offset = static_cast<uint32_t>(output->size());

    AppendU32(output, 0x04034b50U);
    AppendU16(output, 20);
    AppendU16(output, kZipUtf8Flag);
    AppendU16(output, 0);
    AppendU16(output, 0);
    AppendU16(output, 0);
    AppendU32(output, entry.crc32);
    AppendU32(output, static_cast<uint32_t>(entry.data.size()));
    AppendU32(output, static_cast<uint32_t>(entry.data.size()));
    AppendU16(output, static_cast<uint16_t>(entry.name.size()));
    AppendU16(output, 0);
    AppendText(output, entry.name);
    AppendText(output, entry.data);
  }

  if (output->size() > std::numeric_limits<uint32_t>::max()) {
    SetError(error, "diagnostics archive central directory exceeds ZIP limits");
    return false;
  }
  const uint32_t central_offset = static_cast<uint32_t>(output->size());
  for (const ZipEntry& entry : *entries) {
    AppendU32(output, 0x02014b50U);
    AppendU16(output, 20);
    AppendU16(output, 20);
    AppendU16(output, kZipUtf8Flag);
    AppendU16(output, 0);
    AppendU16(output, 0);
    AppendU16(output, 0);
    AppendU32(output, entry.crc32);
    AppendU32(output, static_cast<uint32_t>(entry.data.size()));
    AppendU32(output, static_cast<uint32_t>(entry.data.size()));
    AppendU16(output, static_cast<uint16_t>(entry.name.size()));
    AppendU16(output, 0);
    AppendU16(output, 0);
    AppendU16(output, 0);
    AppendU16(output, 0);
    AppendU32(output, 0);
    AppendU32(output, entry.local_offset);
    AppendText(output, entry.name);
  }

  const uint64_t central_size_value = output->size() - central_offset;
  if (entries->size() > std::numeric_limits<uint16_t>::max() ||
      central_size_value > std::numeric_limits<uint32_t>::max()) {
    SetError(error, "diagnostics archive has too many entries");
    return false;
  }
  const uint16_t count = static_cast<uint16_t>(entries->size());
  AppendU32(output, 0x06054b50U);
  AppendU16(output, 0);
  AppendU16(output, 0);
  AppendU16(output, count);
  AppendU16(output, count);
  AppendU32(output, static_cast<uint32_t>(central_size_value));
  AppendU32(output, central_offset);
  AppendU16(output, 0);

  if (output->size() > kMaximumArchiveBytes) {
    SetError(error, "diagnostics archive exceeds the 16 MiB local export limit");
    return false;
  }
  return true;
}

bool WriteNewFile(
    const std::filesystem::path& path,
    const std::vector<uint8_t>& bytes,
    std::string* error) {
  if (path.empty()) {
    SetError(error, "diagnostics output path is empty");
    return false;
  }

  HANDLE file = CreateFileW(
      path.c_str(),
      GENERIC_WRITE,
      0,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD create_error = GetLastError();
    if (create_error == ERROR_FILE_EXISTS || create_error == ERROR_ALREADY_EXISTS) {
      SetError(error, "diagnostics archive already exists; choose a new output path");
    } else {
      SetError(error, "unable to create diagnostics archive (Windows error " +
                          std::to_string(create_error) + ")");
    }
    return false;
  }

  size_t offset = 0;
  bool success = true;
  while (offset < bytes.size()) {
    const size_t remaining = bytes.size() - offset;
    const DWORD chunk = static_cast<DWORD>(
        std::min<size_t>(remaining, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) || written == 0) {
      success = false;
      break;
    }
    offset += written;
  }
  if (success && !FlushFileBuffers(file)) success = false;
  const DWORD write_error = success ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);

  if (!success) {
    DeleteFileW(path.c_str());
    SetError(error, "unable to write diagnostics archive (Windows error " +
                        std::to_string(write_error) + ")");
    return false;
  }
  return true;
}

}  // namespace

nlohmann::json DiagnosticsRedactor::Redact(
    const nlohmann::json& value,
    const std::vector<std::string>& known_secret_values) {
  return RedactValue(value, known_secret_values);
}

nlohmann::json CollectLocalDiagnosticsSnapshot(
    const DiagnosticsBundleMetadata& metadata) {
  const std::string captured_at = UtcTimestamp();
  const std::string unavailable_reason =
      "one-shot local export has no live session snapshot bridge";
  return {
      {"schema_version", 1},
      {"captured_at_utc", captured_at},
      {"system", {
          {"windows", CollectWindowsInfo()},
          {"cpu", CollectCpuInfo()},
          {"memory", CollectMemoryInfo()},
          {"gpu", CollectGpuInfo()},
          {"display", CollectDisplayInfo()},
      }},
      {"desklink", {
          {"version", metadata.version},
          {"build", metadata.build},
          {"commit_sha", metadata.commit_sha},
          {"runtime_mode", metadata.runtime_mode},
      }},
      {"session", {
          {"current_state", nullptr},
          {"session_generation", nullptr},
          {"peer_generation", nullptr},
          {"recovery_history", json::array()},
          {"connection_timeline", json::array()},
          {"unavailable_reason", unavailable_reason},
      }},
      {"network", {
          {"signal_state", nullptr},
          {"ice_state", nullptr},
          {"candidate_type", nullptr},
          {"path", nullptr},
          {"rtt_ms", nullptr},
          {"packet_loss_percent", nullptr},
          {"jitter_ms", nullptr},
          {"unavailable_reason", unavailable_reason},
      }},
      {"media", {
          {"capture_fps", nullptr},
          {"encode_fps", nullptr},
          {"bitrate_bps", nullptr},
          {"resolution", nullptr},
          {"encoder_backend", nullptr},
          {"gpu_usage_percent", nullptr},
          {"unavailable_reason", unavailable_reason},
      }},
      {"service", {
          {"service_state", ServiceState()},
          {"agent_state", "diagnostics-only"},
          {"restart_count", nullptr},
          {"crash_count", nullptr},
      }},
      {"logs", {
          {"recent_errors", json::array()},
          {"warnings", json::array({unavailable_reason})},
          {"recovery_events", json::array()},
      }},
  };
}

bool ExportDiagnosticsBundle(
    const nlohmann::json& snapshot,
    const std::vector<std::string>& known_secret_values,
    const std::filesystem::path& output_path,
    std::string* error) {
  if (error) error->clear();
  if (!snapshot.is_object() || !snapshot.contains("schema_version") ||
      !snapshot["schema_version"].is_number_integer() ||
      snapshot["schema_version"].get<int>() != 1 ||
      !snapshot.contains("captured_at_utc") ||
      !snapshot["captured_at_utc"].is_string()) {
    SetError(error, "diagnostics snapshot schema is invalid");
    return false;
  }
  for (const std::string_view section : kSectionNames) {
    const auto item = snapshot.find(std::string(section));
    if (item == snapshot.end() || !item->is_object()) {
      SetError(error, "diagnostics snapshot is missing object section: " +
                          std::string(section));
      return false;
    }
  }

  json entry_names = json::array({"manifest.json"});
  for (const std::string_view section : kSectionNames) {
    entry_names.push_back(std::string(section) + ".json");
  }
  const json manifest = {
      {"schema_version", 1},
      {"created_at_utc", snapshot["captured_at_utc"]},
      {"local_export_only", true},
      {"auto_upload", false},
      {"redaction", "DiagnosticsRedactor"},
      {"entries", std::move(entry_names)},
  };

  std::vector<ZipEntry> entries;
  entries.reserve(kSectionNames.size() + 1);
  entries.push_back({
      "manifest.json",
      DiagnosticsRedactor::Redact(manifest, known_secret_values).dump(2) + "\n",
  });
  for (const std::string_view section : kSectionNames) {
    entries.push_back({
        std::string(section) + ".json",
        DiagnosticsRedactor::Redact(
            snapshot.at(std::string(section)), known_secret_values).dump(2) + "\n",
    });
  }

  std::vector<uint8_t> archive;
  if (!BuildStoredZip(&entries, &archive, error)) return false;
  return WriteNewFile(output_path, archive, error);
}

bool ExportLocalDiagnosticsBundle(
    const DiagnosticsBundleMetadata& metadata,
    const std::filesystem::path& output_path,
    std::string* error) {
  return ExportDiagnosticsBundle(
      CollectLocalDiagnosticsSnapshot(metadata), {}, output_path, error);
}

}  // namespace desklink
