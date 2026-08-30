#pragma once

#include <cstdint>
#include <string>
#include <thread>

#include <nlohmann/json_fwd.hpp>

namespace desklink {

class WebRtcSession;

struct HostCapabilitiesV1Input {
  bool secure_attention_available{false};
  std::string secure_attention_reason;
  std::string secure_attention_policy;
  bool clipboard_available{true};
  bool file_transfer_available{true};
  bool system_audio_available{false};
  bool microphone_available{false};
  bool protected_desktop_available{false};
  uint32_t monitor_count{1};
  uint32_t maximum_fps{60};
  uint32_t maximum_width{1920};
  uint32_t maximum_height{1080};
  bool h264_available{true};
};

// Builds the canonical nested HostCapabilitiesV1 control message. This is the
// Windows producer contract; legacy flat capability fields must not be emitted
// by new hosts.
nlohmann::json BuildHostCapabilitiesV1(const HostCapabilitiesV1Input& input);

// Transitional guard for the old main.cpp monitor-state block. New Windows
// sessions publish capabilities independently through HostCapabilitiesPublisher,
// so legacy flat advertisements are discarded before reaching the wire.
bool IsLegacyHostCapabilitiesV1Message(const std::string& text);

class HostCapabilitiesPublisher {
 public:
  explicit HostCapabilitiesPublisher(WebRtcSession* session);
  ~HostCapabilitiesPublisher();

  HostCapabilitiesPublisher(const HostCapabilitiesPublisher&) = delete;
  HostCapabilitiesPublisher& operator=(const HostCapabilitiesPublisher&) = delete;

 private:
  void Run(std::stop_token stop_token);

  WebRtcSession* session_{nullptr};
  std::jthread thread_;
  std::string last_sent_snapshot_;
};

}  // namespace desklink
