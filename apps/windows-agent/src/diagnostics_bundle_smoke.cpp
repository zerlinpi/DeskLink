#include "diagnostics_bundle.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

uint16_t ReadU16(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<uint16_t>(bytes.at(offset)) |
         (static_cast<uint16_t>(bytes.at(offset + 1)) << 8U);
}

uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<uint32_t>(bytes.at(offset)) |
         (static_cast<uint32_t>(bytes.at(offset + 1)) << 8U) |
         (static_cast<uint32_t>(bytes.at(offset + 2)) << 16U) |
         (static_cast<uint32_t>(bytes.at(offset + 3)) << 24U);
}

std::map<std::string, std::string> ReadStoredZipEntries(
    const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("unable to read diagnostics archive");
  std::vector<uint8_t> bytes(
      (std::istreambuf_iterator<char>(stream)),
      std::istreambuf_iterator<char>());

  std::map<std::string, std::string> entries;
  size_t offset = 0;
  while (offset + 30 <= bytes.size() && ReadU32(bytes, offset) == 0x04034b50U) {
    const uint16_t compression = ReadU16(bytes, offset + 8);
    const uint32_t compressed_size = ReadU32(bytes, offset + 18);
    const uint32_t uncompressed_size = ReadU32(bytes, offset + 22);
    const uint16_t name_size = ReadU16(bytes, offset + 26);
    const uint16_t extra_size = ReadU16(bytes, offset + 28);
    const size_t name_offset = offset + 30;
    const size_t data_offset = name_offset + name_size + extra_size;
    if (compression != 0 || compressed_size != uncompressed_size ||
        data_offset + compressed_size > bytes.size()) {
      throw std::runtime_error("diagnostics archive is not a bounded Store ZIP");
    }

    const std::string name(
        reinterpret_cast<const char*>(bytes.data() + name_offset),
        name_size);
    const std::string value(
        reinterpret_cast<const char*>(bytes.data() + data_offset),
        compressed_size);
    if (!entries.emplace(name, value).second) {
      throw std::runtime_error("diagnostics archive contains a duplicate entry");
    }
    offset = data_offset + compressed_size;
  }
  return entries;
}

bool Contains(const std::string& value, const std::string& needle) {
  return value.find(needle) != std::string::npos;
}

}  // namespace

int main() {
  const std::filesystem::path output =
      std::filesystem::temp_directory_path() /
      (L"desklink-diagnostics-smoke-" + std::to_wstring(GetCurrentProcessId()) + L".zip");
  std::error_code remove_error;
  std::filesystem::remove(output, remove_error);

  constexpr char kAccessCode[] = "ACCESS-CODE-SMOKE-SECRET";
  constexpr char kDeviceCredential[] = "dc2.DEVICE-CREDENTIAL-SMOKE-SECRET";
  constexpr char kControllerToken[] = "CONTROLLER-TOKEN-SMOKE-SECRET";
  constexpr char kTurnSecret[] = "TURN-SECRET-SMOKE-SECRET";
  constexpr char kSignalSecret[] = "SIGNAL-SECRET-SMOKE-SECRET";
  constexpr char kPrivateKey[] = "PRIVATE-KEY-SMOKE-SECRET";
  constexpr char kDpapiBlob[] = "DPAPI-BLOB-SMOKE-SECRET";

  const nlohmann::json snapshot = {
      {"schema_version", 1},
      {"captured_at_utc", "2026-01-01T00:00:00Z"},
      {"system", {
          {"windows", {{"product_name", "Windows smoke"}}},
          {"cpu", {{"name", "Smoke CPU"}}},
          {"memory", {{"physical_total_bytes", 1024}}},
          {"gpu", nlohmann::json::array()},
          {"display", nlohmann::json::array()},
          {"access_code", kAccessCode},
      }},
      {"desklink", {
          {"version", "1.0.0"},
          {"build", "smoke"},
          {"commit_sha", "0123456789ab"},
          {"runtime_mode", "cpp-authority;rust-shadow=off"},
          {"device_credential", kDeviceCredential},
      }},
      {"session", {
          {"current_state", nullptr},
          {"session_generation", nullptr},
          {"peer_generation", nullptr},
          {"recovery_history", nlohmann::json::array()},
          {"connection_timeline", nlohmann::json::array()},
          {"controller_token", kControllerToken},
      }},
      {"network", {
          {"signal_state", nullptr},
          {"ice_state", nullptr},
          {"candidate_type", nullptr},
          {"path", nullptr},
          {"rtt_ms", nullptr},
          {"packet_loss_percent", nullptr},
          {"jitter_ms", nullptr},
          {"turn_secret", kTurnSecret},
          {"signal_secret", kSignalSecret},
      }},
      {"media", {
          {"capture_fps", nullptr},
          {"encode_fps", nullptr},
          {"bitrate_bps", nullptr},
          {"resolution", nullptr},
          {"encoder_backend", nullptr},
          {"gpu_usage_percent", nullptr},
      }},
      {"service", {
          {"service_state", "not-installed"},
          {"agent_state", "diagnostics-only"},
          {"restart_count", nullptr},
          {"crash_count", nullptr},
          {"private_key", kPrivateKey},
          {"dpapi_blob", kDpapiBlob},
      }},
      {"logs", {
          {"recent_errors", nlohmann::json::array({
              std::string("Authorization: Bearer ") + kControllerToken,
          })},
          {"warnings", nlohmann::json::array({
              std::string("Access Code: ") + kAccessCode,
          })},
          {"recovery_events", nlohmann::json::array()},
      }},
  };

  std::string error;
  if (!desklink::ExportDiagnosticsBundle(
          snapshot,
          {kAccessCode, kDeviceCredential, kControllerToken, kTurnSecret,
           kSignalSecret, kPrivateKey, kDpapiBlob},
          output,
          &error)) {
    std::cerr << "Diagnostics export failed: " << error << "\n";
    return 1;
  }

  const auto entries = ReadStoredZipEntries(output);
  const std::set<std::string> expected = {
      "manifest.json",
      "system.json",
      "desklink.json",
      "session.json",
      "network.json",
      "media.json",
      "service.json",
      "logs.json",
  };
  std::set<std::string> actual;
  for (const auto& [name, value] : entries) {
    actual.insert(name);
    const auto parsed = nlohmann::json::parse(value, nullptr, false);
    if (parsed.is_discarded()) {
      std::cerr << "Diagnostics entry is not JSON: " << name << "\n";
      return 1;
    }
  }
  if (actual != expected) {
    std::cerr << "Diagnostics archive entries do not match the fixed contract.\n";
    return 1;
  }

  const auto manifest = nlohmann::json::parse(entries.at("manifest.json"));
  if (!manifest.value("local_export_only", false) ||
      manifest.value("auto_upload", true) ||
      manifest.value("redaction", "") != "DiagnosticsRedactor") {
    std::cerr << "Diagnostics manifest lost its local-only safety contract.\n";
    return 1;
  }

  std::ifstream archive_stream(output, std::ios::binary);
  const std::string archive_text(
      (std::istreambuf_iterator<char>(archive_stream)),
      std::istreambuf_iterator<char>());
  for (const char* secret : {
           kAccessCode,
           kDeviceCredential,
           kControllerToken,
           kTurnSecret,
           kSignalSecret,
           kPrivateKey,
           kDpapiBlob,
       }) {
    if (Contains(archive_text, secret)) {
      std::cerr << "Diagnostics archive leaked a protected value.\n";
      return 1;
    }
  }
  if (!Contains(archive_text, "[REDACTED]")) {
    std::cerr << "Diagnostics archive did not apply DiagnosticsRedactor.\n";
    return 1;
  }

  const auto original_size = std::filesystem::file_size(output);
  std::string overwrite_error;
  if (desklink::ExportDiagnosticsBundle(snapshot, {}, output, &overwrite_error) ||
      overwrite_error.empty() ||
      std::filesystem::file_size(output) != original_size) {
    std::cerr << "Diagnostics export must refuse to overwrite an existing archive.\n";
    return 1;
  }

  std::filesystem::remove(output, remove_error);
  std::cout << "DeskLink diagnostics bundle smoke passed.\n";
  return 0;
}
