#include "host_capabilities.h"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace desklink {
namespace {
using nlohmann::json;

json Capability(bool available, const std::string& reason = {}, json metadata = nullptr) {
  json value = {{"available", available}};
  if (!reason.empty()) value["reason"] = reason;
  if (!metadata.is_null() && !metadata.empty()) value["metadata"] = std::move(metadata);
  return value;
}

uint32_t BoundedFps(uint32_t value) {
  return std::clamp<uint32_t>(value, 1, 1000);
}

uint32_t BoundedDimension(uint32_t value) {
  return std::clamp<uint32_t>(value, 1, 16384);
}

}  // namespace

nlohmann::json BuildHostCapabilitiesV1(const HostCapabilitiesV1Input& input) {
  const uint32_t maximum_fps = BoundedFps(input.maximum_fps);
  const uint32_t maximum_width = BoundedDimension(input.maximum_width);
  const uint32_t maximum_height = BoundedDimension(input.maximum_height);

  nlohmann::json secure_attention_metadata = nlohmann::json::object();
  if (!input.secure_attention_policy.empty()) {
    secure_attention_metadata["policy"] = input.secure_attention_policy;
  }

  nlohmann::json codecs = nlohmann::json::array();
  if (input.h264_available) {
    codecs.push_back({
        {"codec", "h264"},
        {"maximumFps", maximum_fps},
        {"maximumResolution", {
            {"width", maximum_width},
            {"height", maximum_height},
        }},
    });
  }

  const bool multi_monitor_available = input.monitor_count > 1;
  const bool high_refresh_available = maximum_fps > 60;

  nlohmann::json capabilities = {
      {"version", 1},
      {"secureAttention", Capability(
          input.secure_attention_available,
          input.secure_attention_reason,
          std::move(secure_attention_metadata))},
      {"clipboard", Capability(
          input.clipboard_available,
          input.clipboard_available ? "" : "unavailable")},
      {"fileTransfer", Capability(
          input.file_transfer_available,
          input.file_transfer_available ? "" : "unavailable")},
      {"systemAudio", Capability(
          input.system_audio_available,
          input.system_audio_available ? "" : "not-implemented")},
      {"microphone", Capability(
          input.microphone_available,
          input.microphone_available ? "" : "not-implemented")},
      {"protectedDesktop", Capability(
          input.protected_desktop_available,
          input.protected_desktop_available ? "" : "not-implemented")},
      {"multiMonitor", Capability(
          multi_monitor_available,
          multi_monitor_available ? "" : "single-monitor")},
      {"highRefresh", Capability(
          high_refresh_available,
          high_refresh_available ? "" : "maximum-fps-not-above-60")},
      {"virtualDisplay", Capability(false, "not-implemented")},
      {"privacyMode", Capability(false, "not-implemented")},
      {"virtualHid", Capability(false, "not-implemented")},
      {"gamepad", Capability(false, "not-implemented")},
      {"codecs", std::move(codecs)},
      {"maximumFps", maximum_fps},
      {"maximumResolution", {
          {"width", maximum_width},
          {"height", maximum_height},
      }},
  };

  return {
      {"t", "host-capabilities"},
      {"version", 1},
      {"capabilities", std::move(capabilities)},
  };
}

bool IsLegacyHostCapabilitiesV1Message(const std::string& text) {
  if (text.find("host-capabilities") == std::string::npos) return false;
  const auto message = nlohmann::json::parse(text, nullptr, false);
  if (message.is_discarded() || !message.is_object()) return false;
  if (message.value("t", "") != "host-capabilities" || message.value("version", 0) != 1) {
    return false;
  }
  if (message.contains("capabilities")) return false;
  return message.contains("secureAttentionAvailable") ||
      message.contains("clipboardAvailable") ||
      message.contains("fileTransferAvailable") ||
      message.contains("audioAvailable") ||
      message.contains("protectedDesktopAvailable");
}

}  // namespace desklink
