#pragma once

#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json_fwd.hpp>
#include <rtc/rtc.hpp>

#include "input_injector.h"

namespace desklink {

struct SessionConfig {
  std::string signal_url{"ws://localhost:8080/ws"};
  std::string device_id{"windows-host"};
  std::string stun_url{"stun:stun.l.google.com:19302"};
  std::string turn_host{"localhost"};
  uint16_t turn_port{3478};
  std::string turn_username{"desklink"};
  std::string turn_password{"CHANGE_ME_NOW"};
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

 private:
  void HandleSignal(const std::string& text);
  void HandleOffer(const std::string& from, const std::string& session, const nlohmann::json& payload);
  void HandleIce(const nlohmann::json& payload);
  void CreatePeer(const std::string& controller, const std::string& session);
  void AttachControlChannel(const std::shared_ptr<rtc::DataChannel>& channel);
  void HandleControl(const std::string& text);
  void SendSignal(const std::string& type, const nlohmann::json& payload);

  SessionConfig config_;
  InputInjector input_;
  std::shared_ptr<rtc::WebSocket> websocket_;
  std::shared_ptr<rtc::PeerConnection> peer_;
  std::shared_ptr<rtc::DataChannel> control_;

  mutable std::mutex mutex_;
  std::string controller_id_;
  std::string session_id_;
};

}  // namespace desklink
