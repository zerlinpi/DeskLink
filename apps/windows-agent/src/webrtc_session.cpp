#include "webrtc_session.h"

#include <chrono>
#include <iostream>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace desklink {
using nlohmann::json;
using namespace std::chrono_literals;

WebRtcSession::WebRtcSession(SessionConfig config) : config_(std::move(config)) {}

WebRtcSession::~WebRtcSession() {
  Stop();
}

void WebRtcSession::Start() {
  rtc::WebSocket::Configuration ws_config;
  ws_config.connectionTimeout = 10s;
  ws_config.pingInterval = 20s;
  ws_config.maxOutstandingPings = 3;

  auto ws = std::make_shared<rtc::WebSocket>(ws_config);
  websocket_ = ws;

  ws->onOpen([this]() {
    std::cout << "Signaling connected as " << config_.device_id << "\n";
  });
  ws->onClosed([]() {
    std::cout << "Signaling connection closed\n";
  });
  ws->onError([](const std::string& error) {
    std::cerr << "Signaling error: " << error << "\n";
  });
  ws->onMessage([this](rtc::message_variant data) {
    if (const auto* text = std::get_if<std::string>(&data)) {
      HandleSignal(*text);
    }
  });

  const char separator = config_.signal_url.find('?') == std::string::npos ? '?' : '&';
  const std::string url = config_.signal_url + separator + "deviceId=" + config_.device_id;
  std::cout << "Connecting signaling: " << url << "\n";
  ws->open(url);
}

void WebRtcSession::Stop() {
  std::shared_ptr<rtc::PeerConnection> peer;
  std::shared_ptr<rtc::WebSocket> ws;
  {
    std::scoped_lock lock(mutex_);
    control_.reset();
    peer = std::move(peer_);
    ws = std::move(websocket_);
    controller_id_.clear();
    session_id_.clear();
  }
  if (peer) peer->close();
  if (ws) ws->close();
}

bool WebRtcSession::connected() const {
  std::scoped_lock lock(mutex_);
  return peer_ && peer_->state() == rtc::PeerConnection::State::Connected;
}

std::string WebRtcSession::controller_id() const {
  std::scoped_lock lock(mutex_);
  return controller_id_;
}

void WebRtcSession::HandleSignal(const std::string& text) {
  const json message = json::parse(text, nullptr, false);
  if (message.is_discarded() || !message.is_object()) return;

  const std::string type = message.value("type", "");
  if (type == "registered") {
    std::cout << "Device registered with signaling server\n";
    return;
  }

  if (type == "offer") {
    const std::string from = message.value("from", "");
    const std::string session = message.value("session", "");
    if (from.empty() || session.empty() || !message.contains("payload")) return;
    HandleOffer(from, session, message["payload"]);
    return;
  }

  if (type == "ice" && message.contains("payload")) {
    HandleIce(message["payload"]);
  }
}

void WebRtcSession::HandleOffer(
    const std::string& from,
    const std::string& session,
    const json& payload) {
  const std::string sdp = payload.value("sdp", "");
  const std::string description_type = payload.value("type", "offer");
  if (sdp.empty()) return;

  std::cout << "Incoming remote-control session from " << from << "\n";
  CreatePeer(from, session);

  std::shared_ptr<rtc::PeerConnection> peer;
  {
    std::scoped_lock lock(mutex_);
    peer = peer_;
  }
  if (peer) {
    peer->setRemoteDescription(rtc::Description(sdp, description_type));
  }
}

void WebRtcSession::HandleIce(const json& payload) {
  const std::string candidate = payload.value("candidate", "");
  const std::string mid = payload.value("sdpMid", "0");
  if (candidate.empty()) return;

  std::shared_ptr<rtc::PeerConnection> peer;
  {
    std::scoped_lock lock(mutex_);
    peer = peer_;
  }
  if (peer) {
    peer->addRemoteCandidate(rtc::Candidate(candidate, mid));
  }
}

void WebRtcSession::CreatePeer(const std::string& controller, const std::string& session) {
  rtc::Configuration rtc_config;
  if (!config_.stun_url.empty()) {
    rtc_config.iceServers.emplace_back(config_.stun_url);
  }
  if (!config_.turn_host.empty() && !config_.turn_username.empty()) {
    rtc_config.iceServers.emplace_back(
        config_.turn_host,
        config_.turn_port,
        config_.turn_username,
        config_.turn_password,
        rtc::IceServer::RelayType::TurnUdp);
    rtc_config.iceServers.emplace_back(
        config_.turn_host,
        config_.turn_port,
        config_.turn_username,
        config_.turn_password,
        rtc::IceServer::RelayType::TurnTcp);
  }

  auto peer = std::make_shared<rtc::PeerConnection>(rtc_config);

  {
    std::scoped_lock lock(mutex_);
    if (peer_) peer_->close();
    peer_ = peer;
    control_.reset();
    controller_id_ = controller;
    session_id_ = session;
  }

  peer->onStateChange([](rtc::PeerConnection::State state) {
    std::cout << "WebRTC state: " << state << "\n";
  });
  peer->onIceStateChange([](rtc::PeerConnection::IceState state) {
    std::cout << "ICE state: " << state << "\n";
  });
  peer->onLocalDescription([this](rtc::Description description) {
    SendSignal(
        description.typeString(),
        json{{"type", description.typeString()}, {"sdp", std::string(description)}});
  });
  peer->onLocalCandidate([this](rtc::Candidate candidate) {
    SendSignal(
        "ice",
        json{{"candidate", std::string(candidate)}, {"sdpMid", candidate.mid()}});
  });
  peer->onDataChannel([this](std::shared_ptr<rtc::DataChannel> channel) {
    if (channel->label() == "control") {
      AttachControlChannel(channel);
    }
  });
}

void WebRtcSession::AttachControlChannel(const std::shared_ptr<rtc::DataChannel>& channel) {
  {
    std::scoped_lock lock(mutex_);
    control_ = channel;
  }

  channel->onOpen([]() {
    std::cout << "Control DataChannel open\n";
  });
  channel->onClosed([]() {
    std::cout << "Control DataChannel closed\n";
  });
  channel->onMessage([this](rtc::message_variant data) {
    if (const auto* text = std::get_if<std::string>(&data)) {
      HandleControl(*text);
    }
  });
}

void WebRtcSession::HandleControl(const std::string& text) {
  const json event = json::parse(text, nullptr, false);
  if (event.is_discarded() || !event.is_object()) return;

  const std::string type = event.value("t", "");
  if (type == "pointer") {
    const std::string kind = event.value("kind", "");
    const double x = event.value("x", 0.0);
    const double y = event.value("y", 0.0);
    const int button = event.value("button", 0);

    if (kind == "move") {
      input_.PointerMove(x, y);
    } else if (kind == "down") {
      input_.PointerMove(x, y);
      input_.PointerButton(button, true);
    } else if (kind == "up") {
      input_.PointerMove(x, y);
      input_.PointerButton(button, false);
    }
    return;
  }

  if (type == "wheel") {
    input_.PointerWheel(event.value("delta", 0));
    return;
  }

  if (type == "key") {
    const std::string kind = event.value("kind", "");
    const std::string code = event.value("code", "");
    if (!code.empty()) {
      input_.Key(code, kind == "down");
    }
  }
}

void WebRtcSession::SendSignal(const std::string& type, const json& payload) {
  std::shared_ptr<rtc::WebSocket> ws;
  std::string controller;
  std::string session;
  {
    std::scoped_lock lock(mutex_);
    ws = websocket_;
    controller = controller_id_;
    session = session_id_;
  }

  if (!ws || !ws->isOpen() || controller.empty() || session.empty()) return;

  const json message = {
      {"type", type},
      {"target", controller},
      {"session", session},
      {"payload", payload},
  };
  ws->send(message.dump());
}

}  // namespace desklink
