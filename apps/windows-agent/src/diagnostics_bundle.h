#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace desklink {

struct DiagnosticsBundleMetadata {
  std::string version;
  std::string build;
  std::string commit_sha;
  std::string runtime_mode;
};

class DiagnosticsRedactor {
 public:
  static nlohmann::json Redact(
      const nlohmann::json& value,
      const std::vector<std::string>& known_secret_values = {});
};

// Collects a one-shot, local-only snapshot. Runtime session metrics are emitted
// as null until the production session authority provides a read-only bridge.
nlohmann::json CollectLocalDiagnosticsSnapshot(
    const DiagnosticsBundleMetadata& metadata);

// Writes a fixed-entry Store ZIP using CREATE_NEW semantics. Every entry passes
// through DiagnosticsRedactor immediately before serialization.
bool ExportDiagnosticsBundle(
    const nlohmann::json& snapshot,
    const std::vector<std::string>& known_secret_values,
    const std::filesystem::path& output_path,
    std::string* error);

bool ExportLocalDiagnosticsBundle(
    const DiagnosticsBundleMetadata& metadata,
    const std::filesystem::path& output_path,
    std::string* error);

}  // namespace desklink
