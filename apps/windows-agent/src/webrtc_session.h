#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json_fwd.hpp>
#include <rtc/rtc.hpp>

#include "input_injector.h"

namespace desklink {

struct NetworkFeedback {
  double loss_ratio{0.0};
  double jitter_ms{0.0};
  double rtt_ms{0.0};
  double decode_fps{0.0};
  double available_incoming_bitrate_bps{0.0};
};

struct SessionConfig {
  std::string signal_url{"ws://localhost:8080/ws"};
  std::string device_id{"windows-host"};
  std::string access_code;
  std::string stun_url{"stun:stun.l.google.com:19302"};
  std::string turn_host{"localhost"};
  uint16_t turn_port{3478};
  std::string turn_username{"desklink"};
  std::string turn_password{"CHANGE_ME_NOW"};
  std::function<void(const NetworkFeedback&)> on_network_feedback;
};

class WebRtcSession {
 public:
  explicit WebRtcSession(SessionConfig config);
  ~WebRtcSession();

  WebRtcSession(const WebRtcSession&) = delete;
  WebRtcSession& operator=(const WebRtcSession&) = delete;

  void Start();
  void Stop();
  bool connected() const;
  std::string controller_id() const;

  // Encoded access units must be Annex-B (00 00 01 / 00 00 00 01 separated NAL units).
  // timestamp100ns is the capture/encode presentation timestamp in 100-ns units.
  bool SendH264AccessUnit(const uint8_t* data, size_t size, uint64_t timestamp100ns);

 private:
  void HandleSignal(const std::string& text);
  void HandleOffer(const std::string& from, const std::string& session, const nlohmann::json& payload);
  void HandleIce(const nlohmann::json& payload);
  void CreatePeer(const std::string& controller, const std::string& session, uint8_t h264_payload_type);
  void AttachControlChannel(const std::shared_ptr<rtc::DataChannel>& channel);
  void HandleControl(const std::string& text);
  void SendSignal(const std::string& type, const nlohmann::json& payload);
  void SendSignalTo(
      const std::string& target,
      const std::string& session,
      const std::string& type,
      const nlohmann::json& payload);

  static uint8_t FindH264PayloadType(const std::string& sdp);
  static bool ConstantTimeEquals(const std::string& left, const std::string& right);

  SessionConfig config_;
  InputInjector input_;
  std::shared_ptr<rtc::WebSocket> websocket_;
  std::shared_ptr<rtc::PeerConnection> peer_;
  std::shared_ptr<rtc::DataChannel> control_;
  std::shared_ptr<rtc::Track> video_track_;
  uint64_t video_timestamp_base100ns_{0};

  mutable std::mutex mutex_;
  std::string controller_id_;
  std::string session_id_;
};

}  // namespace desklink
