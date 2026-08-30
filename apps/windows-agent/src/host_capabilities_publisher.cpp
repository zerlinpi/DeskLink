#include "host_capabilities.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "service_auth_client.h"
#include "webrtc_session.h"

namespace desklink {
using namespace std::chrono_literals;

namespace {

uint32_t EnvUIntOr(
    const char* name,
    uint32_t fallback,
    uint32_t minimum,
    uint32_t maximum) {
  char* value = nullptr;
  size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
    if (value) std::free(value);
    return fallback;
  }

  std::string text(value);
  std::free(value);
  if (text.empty()) return fallback;
  try {
    const unsigned long parsed = std::stoul(text);
    if (parsed >= minimum && parsed <= maximum) {
      return static_cast<uint32_t>(parsed);
    }
  } catch (...) {
  }
  return fallback;
}

HostCapabilitiesV1Input SnapshotHostCapabilities() {
  HostCapabilitiesV1Input input;
  input.maximum_fps = EnvUIntOr("DESKLINK_FPS", 60, 15, 144);
  input.maximum_width = EnvUIntOr("DESKLINK_MAX_WIDTH", 1920, 640, 3840);
  input.maximum_height = EnvUIntOr("DESKLINK_MAX_HEIGHT", 1080, 360, 2160);
  input.monitor_count = std::max<int>(1, GetSystemMetrics(SM_CMONITORS));

  ServiceSecureAttentionStatus sas_status;
  std::string sas_error;
  const bool sas_status_ok = FetchServiceSecureAttentionStatus(&sas_status, &sas_error);
  input.secure_attention_available = sas_status_ok && sas_status.available;
  input.secure_attention_policy = sas_status.policy;
  if (!sas_status_ok) {
    input.secure_attention_reason = "capability-unavailable";
  } else if (!sas_status.broker_configured) {
    input.secure_attention_reason = "service-broker-unavailable";
  } else if (!sas_status.api_available) {
    input.secure_attention_reason = "api-unavailable";
  } else if (!sas_status.policy_readable) {
    input.secure_attention_reason = "policy-read-error";
  } else if (!sas_status.policy_allows_services) {
    input.secure_attention_reason = "policy-not-allowed";
  }

  // These are intentionally conservative until the corresponding independent
  // runtime pipelines exist. A protocol field is not evidence that the feature
  // has been implemented.
  input.clipboard_available = true;
  input.file_transfer_available = true;
  input.system_audio_available = false;
  input.microphone_available = false;
  input.protected_desktop_available = false;
  input.h264_available = true;
  return input;
}

}  // namespace

HostCapabilitiesPublisher::HostCapabilitiesPublisher(WebRtcSession* session)
    : session_(session),
      thread_([this](std::stop_token stop_token) { Run(stop_token); }) {}

HostCapabilitiesPublisher::~HostCapabilitiesPublisher() {
  if (thread_.joinable()) {
    thread_.request_stop();
    thread_.join();
  }
}

void HostCapabilitiesPublisher::Run(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    if (!session_ || !session_->connected()) {
      last_sent_snapshot_.clear();
      std::this_thread::sleep_for(250ms);
      continue;
    }

    const std::string snapshot = BuildHostCapabilitiesV1(SnapshotHostCapabilities()).dump();
    if (snapshot != last_sent_snapshot_) {
      // connected() can become true slightly before the reliable control
      // DataChannel opens. SendControlMessage returns false until the channel is
      // actually ready, so this loop naturally retries without coupling the
      // capability lifecycle to monitor-list-request.
      if (session_->SendControlMessage(snapshot)) {
        last_sent_snapshot_ = snapshot;
        std::cout << "Published HostCapabilitiesV1\n";
      }
    }

    std::this_thread::sleep_for(500ms);
  }
}

}  // namespace desklink
