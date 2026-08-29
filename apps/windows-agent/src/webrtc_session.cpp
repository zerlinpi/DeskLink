#include "webrtc_session.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <regex>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/pacinghandler.hpp>
#include <rtc/plihandler.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtcpnackresponder.hpp>

#include "turn_credential_client.h"

namespace desklink {
using nlohmann::json;
using namespace std::chrono_literals;

namespace {
constexpr rtc::SSRC kVideoSsrc = 42;
constexpr char kVideoCname[] = "desklink-video";
constexpr char kVideoMsid[] = "desklink-stream";

double JsonNumber(const json& object, const char* key, double fallback = 0.0) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_number()) return fallback;
  return it->get<double>();
}

std::string EnvString(const char* name) {
  char* value = nullptr;
  size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
    if (value) std::free(value);
    return {};
  }

  std::string result(value);
  std::free(value);
  return result;
}

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

uint32_t EffectivePacingBitrate(const SessionConfig& config) {
  if (config.media_pacing_bitrate_bps > 0) {
    return std::clamp<uint32_t>(config.media_pacing_bitrate_bps, 500'000, 60'000'000);
  }

  const uint32_t configured_video_bitrate = EnvUIntOr(
      "DESKLINK_BITRATE_BPS",
      12'000'000,
      1'000'000,
      50'000'000);
  const uint64_t with_headroom =
      static_cast<uint64_t>(configured_video_bitrate) * 12 / 10;
  const uint32_t default_pacing = static_cast<uint32_t>(
      std::min<uint64_t>(with_headroom, 60'000'000));
  return EnvUIntOr(
      "DESKLINK_PACING_BPS",
      default_pacing,
      500'000,
      60'000'000);
}
}  // namespace

WebRtcSession::WebRtcSession(SessionConfig config) : config_(std::move(config)) {}

WebRtcSession::~WebRtcSession() {
  Stop();
}

void WebRtcSession::Start() {
  bool expected_stopped = true;
  if (!stopping_.compare_exchange_strong(expected_stopped, false)) return;

  reconnect_attempt_.store(0, std::memory_order_relaxed);
  {
    std::scoped_lock lock(reconnect_mutex_);
    reconnect_requested_ = false;
  }

  reconnect_thread_ = std::jthread([this](std::stop_token stop_token) {
    SignalingReconnectLoop(stop_token);
  });
  ConnectSignaling();
}

void WebRtcSession::ConnectSignaling() {
  if (stopping_.load(std::memory_order_relaxed)) return;

  rtc::WebSocket::Configuration ws_config;
  ws_config.connectionTimeout = 10s;
  ws_config.pingInterval = 20s;
  ws_config.maxOutstandingPings = 3;

  auto ws = std::make_shared<rtc::WebSocket>(ws_config);
  std::weak_ptr<rtc::WebSocket> weak_ws = ws;

  ws->onOpen([this, weak_ws]() {
    auto opened = weak_ws.lock();
    if (!opened || stopping_.load(std::memory_order_relaxed)) return;

    {
      std::scoped_lock lock(mutex_);
      if (websocket_ != opened) return;
    }

    reconnect_attempt_.store(0, std::memory_order_relaxed);
    std::cout << "Signaling connected as " << config_.device_id << "\n";
  });
  ws->onClosed([this, weak_ws]() {
    auto closed = weak_ws.lock();
    bool was_current = false;
    {
      std::scoped_lock lock(mutex_);
      if (closed && websocket_ == closed) {
        websocket_.reset();
        was_current = true;
      }
    }

    if (!was_current) return;
    std::cout << "Signaling connection closed\n";
    if (!stopping_.load(std::memory_order_relaxed)) {
      RequestSignalingReconnect();
    }
  });
  ws->onError([this, weak_ws](const std::string& error) {
    auto failed = weak_ws.lock();
    if (!failed) return;
    {
      std::scoped_lock lock(mutex_);
      if (websocket_ != failed) return;
    }
    std::cerr << "Signaling error: " << error << "\n";
  });
  ws->onMessage([this, weak_ws](rtc::message_variant data) {
    auto source = weak_ws.lock();
    if (!source) return;
    {
      std::scoped_lock lock(mutex_);
      if (websocket_ != source) return;
    }
    if (const auto* text = std::get_if<std::string>(&data)) {
      HandleSignal(*text);
    }
  });

  {
    std::scoped_lock lock(mutex_);
    if (stopping_.load(std::memory_order_relaxed)) return;
    websocket_ = ws;
  }

  const char separator = config_.signal_url.find('?') == std::string::npos ? '?' : '&';
  std::string url = config_.signal_url + separator + "deviceId=" + config_.device_id;
  const std::string signal_auth_token = EnvString("DESKLINK_SIGNAL_AUTH_TOKEN");
  if (!signal_auth_token.empty()) {
    url += "&auth=" + signal_auth_token;
  }
  std::cout << "Connecting signaling as " << config_.device_id
            << (signal_auth_token.empty() ? " without registration token" : " with registration token")
            << "\n";
  ws->open(url);
}

void WebRtcSession::RequestSignalingReconnect() {
  if (stopping_.load(std::memory_order_relaxed)) return;
  {
    std::scoped_lock lock(reconnect_mutex_);
    if (reconnect_requested_) return;
    reconnect_requested_ = true;
  }
  reconnect_cv_.notify_one();
}

void WebRtcSession::SignalingReconnectLoop(std::stop_token stop_token) {
  std::unique_lock lock(reconnect_mutex_);
  while (!stop_token.stop_requested() && !stopping_.load(std::memory_order_relaxed)) {
    reconnect_cv_.wait(lock, [this, &stop_token]() {
      return reconnect_requested_ || stop_token.stop_requested() ||
             stopping_.load(std::memory_order_relaxed);
    });

    if (stop_token.stop_requested() || stopping_.load(std::memory_order_relaxed)) return;
    reconnect_requested_ = false;

    const uint32_t attempt = reconnect_attempt_.fetch_add(1, std::memory_order_relaxed);
    const uint32_t exponent = std::min<uint32_t>(attempt, 5);
    const uint32_t delay_seconds = std::min<uint32_t>(30, 1u << exponent);
    lock.unlock();

    std::cout << "Reconnecting signaling in " << delay_seconds << "s\n";
    for (uint32_t tick = 0; tick < delay_seconds * 10; ++tick) {
      if (stop_token.stop_requested() || stopping_.load(std::memory_order_relaxed)) return;
      std::this_thread::sleep_for(100ms);
    }

    if (!stop_token.stop_requested() && !stopping_.load(std::memory_order_relaxed)) {
      ConnectSignaling();
    }

    lock.lock();
  }
}

void WebRtcSession::Stop() {
  if (stopping_.exchange(true, std::memory_order_relaxed)) return;

  input_.ReleaseAll();
  {
    std::scoped_lock lock(reconnect_mutex_);
    reconnect_requested_ = false;
  }
  if (reconnect_thread_.joinable()) {
    reconnect_thread_.request_stop();
    reconnect_cv_.notify_all();
  }

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

  if (reconnect_thread_.joinable()) {
    reconnect_thread_.join();
  }
}

bool WebRtcSession::connected() const {
  std::scoped_lock lock(mutex_);
  return peer_ && peer_->state() == rtc::PeerConnection::State::Connected;
}

std::string WebRtcSession::controller_id() const {
  std::scoped_lock lock(mutex_);
  return controller_id_;
}

void WebRtcSession::SetControlledDesktopRect(
    long left,
    long top,
    long width,
    long height) {
  input_.SetDesktopRect(left, top, width, height);
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
    const std::string from = message.value("from", "");
    const std::string session = message.value("session", "");
    bool authorized_session = false;
    {
      std::scoped_lock lock(mutex_);
      authorized_session = !from.empty() && !session.empty() &&
                           from == controller_id_ && session == session_id_;
    }
    if (authorized_session) {
      HandleIce(message["payload"]);
    }
  }
}

bool WebRtcSession::ConstantTimeEquals(
    const std::string& left,
    const std::string& right) {
  const size_t longest = std::max(left.size(), right.size());
  unsigned char difference = static_cast<unsigned char>(left.size() ^ right.size());
  for (size_t i = 0; i < longest; ++i) {
    const unsigned char a = i < left.size() ? static_cast<unsigned char>(left[i]) : 0;
    const unsigned char b = i < right.size() ? static_cast<unsigned char>(right[i]) : 0;
    difference |= static_cast<unsigned char>(a ^ b);
  }
  return difference == 0;
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
  if (!payload.is_object()) return;

  if (config_.access_code.empty()) {
    std::cerr << "Rejected remote session: DESKLINK_ACCESS_CODE is not configured\n";
    SendSignalTo(
        from,
        session,
        "auth-rejected",
        json{{"reason", "host-unconfigured"}});
    return;
  }

  const std::string supplied_access_code = payload.value("accessCode", "");
  if (!ConstantTimeEquals(supplied_access_code, config_.access_code)) {
    std::cerr << "Rejected remote session from " << from << ": invalid access code\n";
    SendSignalTo(
        from,
        session,
        "auth-rejected",
        json{{"reason", "invalid-access-code"}});
    return;
  }

  const std::string sdp = payload.value("sdp", "");
  const std::string description_type = payload.value("type", "offer");
  if (sdp.empty()) return;

  const uint8_t h264_payload_type = FindH264PayloadType(sdp);
  if (h264_payload_type == 0) {
    std::cerr << "Controller offer does not contain H264; control will connect without video\n";
  } else {
    std::cout << "Negotiated H264 RTP payload type " << static_cast<int>(h264_payload_type) << "\n";
  }

  std::shared_ptr<rtc::PeerConnection> existing_peer;
  {
    std::scoped_lock lock(mutex_);
    if (peer_ && from == controller_id_ && session == session_id_ &&
        peer_->state() != rtc::PeerConnection::State::Closed) {
      existing_peer = peer_;
    }
  }

  if (existing_peer) {
    // ICE restarts and network-path changes must renegotiate the already-authorized
    // session in place. Replacing PeerConnection here would unnecessarily tear down
    // the current DataChannels and can invalidate a browser-side ICE restart.
    std::cout << "Renegotiating authorized session from " << from << "\n";
    existing_peer->setRemoteDescription(rtc::Description(sdp, description_type));
    return;
  }

  std::cout << "Authorized remote-control session from " << from << "\n";
  input_.ReleaseAll();
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

  std::string turn_username = config_.turn_username;
  std::string turn_password = config_.turn_password;
  const std::string credential_endpoint = EnvString("DESKLINK_TURN_CREDENTIALS_URL");
  if (!credential_endpoint.empty()) {
    RuntimeTurnCredentials credentials;
    std::string credential_error;
    const std::string signal_auth_token = EnvString("DESKLINK_SIGNAL_AUTH_TOKEN");
    if (FetchRuntimeTurnCredentials(
            credential_endpoint,
            config_.device_id,
            signal_auth_token,
            &credentials,
            &credential_error)) {
      turn_username = std::move(credentials.username);
      turn_password = std::move(credentials.password);
      std::cout << "Loaded temporary TURN credentials for this session; expires at "
                << credentials.expires_at << "\n";
    } else {
      std::cerr << "Temporary TURN credential fetch failed: " << credential_error << "\n";
      if (EnvString("DESKLINK_TURN_RUNTIME_REQUIRED") == "1") {
        turn_username.clear();
        turn_password.clear();
        std::cerr << "TURN disabled because runtime credentials are required\n";
      } else {
        std::cerr << "Falling back to configured static TURN credentials\n";
      }
    }
  }

  if (!config_.turn_host.empty() && !turn_username.empty() && !turn_password.empty()) {
    rtc_config.iceServers.emplace_back(
        config_.turn_host,
        config_.turn_port,
        turn_username,
        turn_password,
        rtc::IceServer::RelayType::TurnUdp);
    rtc_config.iceServers.emplace_back(
        config_.turn_host,
        config_.turn_port,
        turn_username,
        turn_password,
        rtc::IceServer::RelayType::TurnTcp);

    const uint32_t turn_tls_port = EnvUIntOr(
        "DESKLINK_TURN_TLS_PORT",
        5349,
        0,
        65535);
    if (turn_tls_port > 0) {
      rtc_config.iceServers.emplace_back(
          config_.turn_host,
          static_cast<uint16_t>(turn_tls_port),
          turn_username,
          turn_password,
          rtc::IceServer::RelayType::TurnTls);
      std::cout << "TURN TLS fallback enabled on port " << turn_tls_port << "\n";
    }
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

    if (config_.on_keyframe_requested) {
      const auto on_keyframe_requested = config_.on_keyframe_requested;
      packetizer->addToChain(std::make_shared<rtc::PliHandler>([on_keyframe_requested]() {
        on_keyframe_requested();
      }));
    }

    const uint32_t pacing_bps = EffectivePacingBitrate(config_);
    const uint32_t pacing_interval_ms = std::clamp<uint32_t>(
        config_.media_pacing_interval_ms,
        1,
        20);
    if (pacing_bps > 0) {
      packetizer->addToChain(std::make_shared<rtc::PacingHandler>(
          static_cast<double>(pacing_bps),
          std::chrono::milliseconds(pacing_interval_ms)));
      std::cout << "RTP pacing: " << (pacing_bps / 1'000'000.0)
                << " Mbps, " << pacing_interval_ms << " ms interval\n";
    }

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
  channel->onClosed([this, label]() {
    if (label == "control") input_.ReleaseAll();
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
  if (type == "release-all") {
    input_.ReleaseAll();
    return;
  }

  if (type == "telemetry") {
    if (config_.on_network_feedback) {
      NetworkFeedback feedback;
      feedback.loss_ratio = std::clamp(JsonNumber(event, "lossPct") / 100.0, 0.0, 1.0);
      feedback.rtt_ms = std::clamp(JsonNumber(event, "rttMs"), 0.0, 5000.0);
      feedback.jitter_ms = std::clamp(JsonNumber(event, "jitterMs"), 0.0, 5000.0);
      feedback.decode_fps = std::clamp(JsonNumber(event, "decodeFps"), 0.0, 240.0);
      feedback.available_incoming_bitrate_bps = std::max(
          0.0,
          JsonNumber(event, "availableIncomingBitrate"));
      config_.on_network_feedback(feedback);
    }
    return;
  }

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
  std::string controller;
  std::string session;
  {
    std::scoped_lock lock(mutex_);
    controller = controller_id_;
    session = session_id_;
  }
  SendSignalTo(controller, session, type, payload);
}

void WebRtcSession::SendSignalTo(
    const std::string& target,
    const std::string& session,
    const std::string& type,
    const json& payload) {
  std::shared_ptr<rtc::WebSocket> ws;
  {
    std::scoped_lock lock(mutex_);
    ws = websocket_;
  }

  if (!ws || !ws->isOpen() || target.empty() || session.empty()) return;

  const json message = {
      {"type", type},
      {"target", target},
      {"session", session},
      {"payload", payload},
  };
  ws->send(message.dump());
}

}  // namespace desklink
