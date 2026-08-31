#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include <nlohmann/json_fwd.hpp>
#include <rtc/rtc.hpp>

#include "host_capabilities.h"
#include "input_channel_authority.h"
#include "input_injector.h"
#include "video_policy.h"

namespace desklink {

class FileTransferReceiver;

struct NetworkFeedback {
  double loss_ratio{0.0};
  double jitter_ms{0.0};
  double rtt_ms{0.0};
  double control_rtt_ms{0.0};
  double decode_fps{0.0};
  double available_incoming_bitrate_bps{0.0};
};

enum class VideoProfile : uint8_t {
  Auto = 0,
  Original = 1,
  High = 2,
  Clear = 3,
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
  // Zero derives the pacing budget from DESKLINK_BITRATE_BPS (default 12 Mbps).
  uint32_t media_pacing_bitrate_bps{0};
  uint32_t media_pacing_interval_ms{5};
  std::function<void(const NetworkFeedback&)> on_network_feedback;
  std::function<void(VideoProfile)> on_video_profile_requested;
  std::function<void(AdaptationMode)> on_adaptation_mode_requested;
  std::function<void()> on_monitor_state_requested;
  std::function<void(uint32_t)> on_monitor_switch_requested;
  std::function<void()> on_keyframe_requested;
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
  void SetControlledDesktopRect(long left, long top, long width, long height);

  // Sends an application event to the currently authorized reliable control
  // channel. Returns false when no control channel is open.
  bool SendControlMessage(const std::string& text);

  // Encoded access units must be Annex-B (00 00 01 / 00 00 00 01 separated NAL units).
  // timestamp100ns is the capture/encode presentation timestamp in 100-ns units.
  bool SendH264AccessUnit(const uint8_t* data, size_t size, uint64_t timestamp100ns);

 private:
  struct PendingAccessChallenge {
    std::string nonce;
    std::chrono::steady_clock::time_point expires{};
  };

  void ConnectSignaling();
  void RequestSignalingReconnect();
  void SignalingReconnectLoop(std::stop_token stop_token);
  void HandleSignal(const std::string& text);
  void HandleAuthRequest(const std::string& from, const std::string& session);
  void HandleAuthProof(
      const std::string& from,
      const std::string& session,
      const nlohmann::json& payload);
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
  InputChannelAuthority input_channel_authority_;
  std::shared_ptr<FileTransferReceiver> file_transfer_receiver_;
  std::shared_ptr<rtc::Track> video_track_;
  uint64_t video_timestamp_base100ns_{0};

  mutable std::mutex mutex_;
  std::string controller_id_;
  std::string session_id_;
  std::unordered_map<std::string, PendingAccessChallenge> pending_access_challenges_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> authorized_offer_sessions_;

  std::atomic_bool stopping_{true};
  std::atomic_bool registration_revoked_{false};
  std::atomic_uint32_t reconnect_attempt_{0};
  std::mutex reconnect_mutex_;
  std::condition_variable reconnect_cv_;
  bool reconnect_requested_{false};
  std::jthread reconnect_thread_;

  // Keep this member last: its worker may call connected()/SendControlMessage(),
  // so every session synchronization primitive must already be constructed.
  HostCapabilitiesPublisher host_capabilities_publisher_{this};
};

}  // namespace desklink
