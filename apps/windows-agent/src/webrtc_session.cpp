#include "webrtc_session.h"

#include <chrono>
#include <iostream>
#include <regex>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtcpnackresponder.hpp>

namespace desklink {
using nlohmann::json;
using namespace std::chrono_literals;

namespace {
constexpr rtc::SSRC kVideoSsrc = 42;
constexpr char kVideoCname[] = "desklink-video";
constexpr char kVideoMsid[] = "desklink-stream";
}  // namespace

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
    video_track_.reset();
    video_timestamp_base100ns_ = 0;
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

bool WebRtcSession::SendH264AccessUnit(
    const uint8_t* data,
    size_t size,
    uint64_t timestamp100ns) {
  if (!data || size == 0) return false;

  std::shared_ptr<rtc::Track> track;
  uint64_t base = 0;
  {
    std::scoped_lock lock(mutex_);
    track = video_track_;
    if (!track || !track->isOpen()) return false;
    if (video_timestamp_base100ns_ == 0) {
      video_timestamp_base100ns_ = timestamp100ns;
    }
    base = video_timestamp_base100ns_;
  }

  const uint64_t relative = timestamp100ns >= base ? timestamp100ns - base : 0;
  const auto pts = std::chrono::duration<double>(static_cast<double>(relative) / 10'000'000.0);

  try {
    track->sendFrame(
        reinterpret_cast<const rtc::byte*>(data),
        size,
        rtc::FrameInfo(pts));
    return true;
  } catch (const std::exception& error) {
    std::cerr << "H264 WebRTC send failed: " << error.what() << "\n";
    return false;
  }
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

uint8_t WebRtcSession::FindH264PayloadType(const std::string& sdp) {
  static const std::regex pattern(
      R"(a=rtpmap:([0-9]+)[ \t]+H264/90000)",
      std::regex_constants::icase);
  std::smatch match;
  if (!std::regex_search(sdp, match, pattern) || match.size() < 2) return 0;

  try {
    const int value = std::stoi(match[1].str());
    if (value > 0 && value <= 127) return static_cast<uint8_t>(value);
  } catch (...) {
  }
  return 0;
}

void WebRtcSession::HandleOffer(
    const std::string& from,
    const std::string& session,
    const json& payload) {
  const std::string sdp = payload.value("sdp", "");
  const std::string description_type = payload.value("type", "offer");
  if (sdp.empty()) return;

  const uint8_t h264_payload_type = FindH264PayloadType(sdp);
  if (h264_payload_type == 0) {
    std::cerr << "Controller offer does not contain H264; control will connect without video\n";
  } else {
    std::cout << "Negotiated H264 RTP payload type " << static_cast<int>(h264_payload_type) << "\n";
  }

  std::cout << "Incoming remote-control session from " << from << "\n";
  CreatePeer(from, session, h264_payload_type);

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
  if (!payload.is_object()) return;
  const auto candidate_it = payload.find("candidate");
  if (candidate_it == payload.end() || !candidate_it->is_string()) return;
  const std::string candidate = candidate_it->get<std::string>();
  if (candidate.empty()) return;

  std::string mid = "0";
  const auto mid_it = payload.find("sdpMid");
  if (mid_it != payload.end() && mid_it->is_string()) {
    mid = mid_it->get<std::string>();
  }

  std::shared_ptr<rtc::PeerConnection> peer;
  {
    std::scoped_lock lock(mutex_);
    peer = peer_;
  }
  if (peer) {
    peer->addRemoteCandidate(rtc::Candidate(candidate, mid));
  }
}

void WebRtcSession::CreatePeer(
    const std::string& controller,
    const std::string& session,
    uint8_t h264_payload_type) {
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
  std::shared_ptr<rtc::Track> video_track;

  if (h264_payload_type != 0) {
    rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
    media.addH264Codec(h264_payload_type);
    media.addSSRC(kVideoSsrc, kVideoCname, kVideoMsid, kVideoCname);
    video_track = peer->addTrack(media);

    auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
        kVideoSsrc,
        kVideoCname,
        h264_payload_type,
        rtc::H264RtpPacketizer::ClockRate);
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::StartSequence,
        rtp_config);
    packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp_config));
    packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
    video_track->setMediaHandler(packetizer);
    video_track->onOpen([]() {
      std::cout << "H264 video track open\n";
    });
    video_track->onClosed([]() {
      std::cout << "H264 video track closed\n";
    });
  }

  std::shared_ptr<rtc::PeerConnection> previous;
  {
    std::scoped_lock lock(mutex_);
    previous = std::move(peer_);
    peer_ = peer;
    control_.reset();
    video_track_ = video_track;
    video_timestamp_base100ns_ = 0;
    controller_id_ = controller;
    session_id_ = session;
  }
  if (previous) previous->close();

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
    if (channel->label() == "control" || channel->label() == "pointer") {
      AttachControlChannel(channel);
    }
  });
}

void WebRtcSession::AttachControlChannel(const std::shared_ptr<rtc::DataChannel>& channel) {
  if (channel->label() == "control") {
    std::scoped_lock lock(mutex_);
    control_ = channel;
  }

  const std::string label = channel->label();
  channel->onOpen([label]() {
    std::cout << label << " DataChannel open\n";
  });
  channel->onClosed([label]() {
    std::cout << label << " DataChannel closed\n";
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
