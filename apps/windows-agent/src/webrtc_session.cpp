#include "webrtc_session.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <regex>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/pacinghandler.hpp>
#include <rtc/plihandler.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtcpnackresponder.hpp>

#include "clipboard_win32.h"
#include "signal_token_client.h"
#include "turn_credential_client.h"

namespace desklink {
using nlohmann::json;
using namespace std::chrono_literals;

namespace {
constexpr rtc::SSRC kVideoSsrc = 42;
constexpr char kVideoCname[] = "desklink-video";
constexpr char kVideoMsid[] = "desklink-stream";
constexpr auto kAccessChallengeLifetime = 15s;
constexpr size_t kMaxPendingAccessChallenges = 64;
constexpr char kAccessProofAlgorithm[] = "hmac-sha256-v1";
constexpr size_t kMaxControlMessageBytes = 512 * 1024;
constexpr size_t kMaxControlRequestIdBytes = 128;

double JsonNumber(const json& object, const char* key, double fallback = 0.0) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_number()) return fallback;
  return it->get<double>();
}

bool ValidRequestId(const std::string& request_id) {
  return !request_id.empty() && request_id.size() <= kMaxControlRequestIdBytes;
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

bool ResolveSignalAuthToken(
    const SessionConfig& config,
    std::string* token,
    std::string* warning_or_error) {
  if (!token) return false;
  *token = EnvString("DESKLINK_SIGNAL_AUTH_TOKEN");
  if (warning_or_error) warning_or_error->clear();

  const std::string endpoint = EnvString("DESKLINK_SIGNAL_TOKEN_URL");
  if (endpoint.empty()) return true;

  RuntimeSignalToken runtime_token;
  std::string fetch_error;
  if (FetchRuntimeSignalToken(
          endpoint,
          config.device_id,
          EnvString("DESKLINK_DEVICE_CREDENTIAL"),
          &runtime_token,
          &fetch_error)) {
    *token = std::move(runtime_token.token);
    std::cout << "Loaded short-lived signaling token; expires at "
              << runtime_token.expires_at << "\n";
    return true;
  }

  if (warning_or_error) {
    *warning_or_error = "runtime signaling token fetch failed: " + fetch_error;
  }
  if (EnvString("DESKLINK_SIGNAL_TOKEN_REQUIRED") == "1") {
    token->clear();
    return false;
  }
  return true;
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

std::string HexEncode(const UCHAR* bytes, size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result(size * 2, '0');
  for (size_t i = 0; i < size; ++i) {
    result[i * 2] = kHex[(bytes[i] >> 4) & 0x0f];
    result[i * 2 + 1] = kHex[bytes[i] & 0x0f];
  }
  return result;
}

bool RandomNonceHex(std::string* nonce) {
  if (!nonce) return false;
  std::vector<UCHAR> bytes(32);
  const NTSTATUS status = BCryptGenRandom(
      nullptr,
      bytes.data(),
      static_cast<ULONG>(bytes.size()),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (!BCRYPT_SUCCESS(status)) return false;
  *nonce = HexEncode(bytes.data(), bytes.size());
  SecureZeroMemory(bytes.data(), bytes.size());
  return true;
}

bool HmacSha256Hex(
    const std::string& key,
    const std::string& message,
    std::string* digest_hex) {
  if (!digest_hex || key.empty() ||
      key.size() > std::numeric_limits<ULONG>::max() ||
      message.size() > std::numeric_limits<ULONG>::max()) {
    return false;
  }

  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD object_length = 0;
  DWORD hash_length = 0;
  DWORD bytes_written = 0;
  std::vector<UCHAR> hash_object;
  std::vector<UCHAR> digest;

  NTSTATUS status = BCryptOpenAlgorithmProvider(
      &algorithm,
      BCRYPT_SHA256_ALGORITHM,
      nullptr,
      BCRYPT_ALG_HANDLE_HMAC_FLAG);
  if (!BCRYPT_SUCCESS(status)) return false;

  auto cleanup = [&]() {
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!hash_object.empty()) SecureZeroMemory(hash_object.data(), hash_object.size());
    if (!digest.empty()) SecureZeroMemory(digest.data(), digest.size());
  };

  status = BCryptGetProperty(
      algorithm,
      BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<PUCHAR>(&object_length),
      sizeof(object_length),
      &bytes_written,
      0);
  if (!BCRYPT_SUCCESS(status) || object_length == 0) {
    cleanup();
    return false;
  }
  status = BCryptGetProperty(
      algorithm,
      BCRYPT_HASH_LENGTH,
      reinterpret_cast<PUCHAR>(&hash_length),
      sizeof(hash_length),
      &bytes_written,
      0);
  if (!BCRYPT_SUCCESS(status) || hash_length == 0) {
    cleanup();
    return false;
  }

  hash_object.resize(object_length);
  digest.resize(hash_length);
  status = BCryptCreateHash(
      algorithm,
      &hash,
      hash_object.data(),
      static_cast<ULONG>(hash_object.size()),
      const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(key.data())),
      static_cast<ULONG>(key.size()),
      0);
  if (!BCRYPT_SUCCESS(status)) {
    cleanup();
    return false;
  }
  status = BCryptHashData(
      hash,
      const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(message.data())),
      static_cast<ULONG>(message.size()),
      0);
  if (!BCRYPT_SUCCESS(status)) {
    cleanup();
    return false;
  }
  status = BCryptFinishHash(
      hash,
      digest.data(),
      static_cast<ULONG>(digest.size()),
      0);
  if (!BCRYPT_SUCCESS(status)) {
    cleanup();
    return false;
  }

  *digest_hex = HexEncode(digest.data(), digest.size());
  cleanup();
  return true;
}

std::string AccessSessionKey(const std::string& controller, const std::string& session) {
  return controller + "\n" + session;
}

std::string AccessProofMessage(
    const std::string& controller,
    const std::string& host,
    const std::string& session,
    const std::string& nonce) {
  return "DeskLink access proof v1\n" + controller + "\n" + host + "\n" + session + "\n" + nonce;
}
}  // namespace

WebRtcSession::WebRtcSession(SessionConfig config) : config_(std::move(config)) {}

WebRtcSession::~WebRtcSession() {
  Stop();
}

void WebRtcSession::Start() {
  bool expected_stopped = true;
  if (!stopping_.compare_exchange_strong(expected_stopped, false)) return;

  registration_revoked_.store(false, std::memory_order_relaxed);
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
  if (stopping_.load(std::memory_order_relaxed) ||
      registration_revoked_.load(std::memory_order_relaxed)) {
    return;
  }

  std::string signal_auth_token;
  std::string token_error;
  if (!ResolveSignalAuthToken(config_, &signal_auth_token, &token_error)) {
    std::cerr << token_error << "; signaling connection deferred\n";
    RequestSignalingReconnect();
    return;
  }
  if (!token_error.empty()) {
    std::cerr << token_error << "; using configured signaling token fallback\n";
  }

  rtc::WebSocket::Configuration ws_config;
  ws_config.connectionTimeout = 10s;
  ws_config.pingInterval = 20s;
  ws_config.maxOutstandingPings = 3;

  auto ws = std::make_shared<rtc::WebSocket>(ws_config);
  std::weak_ptr<rtc::WebSocket> weak_ws = ws;

  ws->onOpen([this, weak_ws]() {
    auto opened = weak_ws.lock();
    if (!opened || stopping_.load(std::memory_order_relaxed) ||
        registration_revoked_.load(std::memory_order_relaxed)) {
      if (opened) opened->close();
      return;
    }

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
    if (!stopping_.load(std::memory_order_relaxed) &&
        !registration_revoked_.load(std::memory_order_relaxed)) {
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
    if (stopping_.load(std::memory_order_relaxed) ||
        registration_revoked_.load(std::memory_order_relaxed)) {
      return;
    }
    websocket_ = ws;
  }

  const char separator = config_.signal_url.find('?') == std::string::npos ? '?' : '&';
  std::string url = config_.signal_url + separator + "deviceId=" + config_.device_id;
  if (!signal_auth_token.empty()) {
    url += "&auth=" + signal_auth_token;
  }
  std::cout << "Connecting signaling as " << config_.device_id
            << (signal_auth_token.empty() ? " without registration token" : " with registration token")
            << "\n";
  ws->open(url);
}

void WebRtcSession::RequestSignalingReconnect() {
  if (stopping_.load(std::memory_order_relaxed) ||
      registration_revoked_.load(std::memory_order_relaxed)) {
    return;
  }
  {
    std::scoped_lock lock(reconnect_mutex_);
    if (reconnect_requested_) return;
    reconnect_requested_ = true;
  }
  reconnect_cv_.notify_one();
}

void WebRtcSession::SignalingReconnectLoop(std::stop_token stop_token) {
  std::unique_lock lock(reconnect_mutex_);
  while (!stop_token.stop_requested() &&
         !stopping_.load(std::memory_order_relaxed) &&
         !registration_revoked_.load(std::memory_order_relaxed)) {
    reconnect_cv_.wait(lock, [this, &stop_token]() {
      return reconnect_requested_ || stop_token.stop_requested() ||
             stopping_.load(std::memory_order_relaxed) ||
             registration_revoked_.load(std::memory_order_relaxed);
    });

    if (stop_token.stop_requested() ||
        stopping_.load(std::memory_order_relaxed) ||
        registration_revoked_.load(std::memory_order_relaxed)) {
      return;
    }
    reconnect_requested_ = false;

    const uint32_t attempt = reconnect_attempt_.fetch_add(1, std::memory_order_relaxed);
    const uint32_t exponent = std::min<uint32_t>(attempt, 5);
    const uint32_t delay_seconds = std::min<uint32_t>(30, 1u << exponent);
    lock.unlock();

    std::cout << "Reconnecting signaling in " << delay_seconds << "s\n";
    for (uint32_t tick = 0; tick < delay_seconds * 10; ++tick) {
      if (stop_token.stop_requested() ||
          stopping_.load(std::memory_order_relaxed) ||
          registration_revoked_.load(std::memory_order_relaxed)) {
        return;
      }
      std::this_thread::sleep_for(100ms);
    }

    if (!stop_token.stop_requested() &&
        !stopping_.load(std::memory_order_relaxed) &&
        !registration_revoked_.load(std::memory_order_relaxed)) {
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
    pending_access_challenges_.clear();
    authorized_offer_sessions_.clear();
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

bool WebRtcSession::SendControlMessage(const std::string& text) {
  if (text.empty() || text.size() > kMaxControlMessageBytes) return false;

  std::shared_ptr<rtc::DataChannel> control;
  {
    std::scoped_lock lock(mutex_);
    control = control_;
  }
  if (!control || !control->isOpen()) return false;

  try {
    control->send(text);
    return true;
  } catch (const std::exception& error) {
    std::cerr << "Control DataChannel send failed: " << error.what() << "\n";
    return false;
  }
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

  if (type == "device-revoked") {
    const std::string revoked_target = message.value("target", "");
    if (revoked_target != config_.device_id) {
      std::cerr << "Ignored device-revoked for unexpected target " << revoked_target << "\n";
      return;
    }

    registration_revoked_.store(true, std::memory_order_relaxed);
    input_.ReleaseAll();
    {
      std::scoped_lock lock(reconnect_mutex_);
      reconnect_requested_ = false;
    }
    reconnect_cv_.notify_all();

    std::shared_ptr<rtc::PeerConnection> peer;
    {
      std::scoped_lock lock(mutex_);
      control_.reset();
      video_track_.reset();
      video_timestamp_base100ns_ = 0;
      peer = std::move(peer_);
      controller_id_.clear();
      session_id_.clear();
      pending_access_challenges_.clear();
      authorized_offer_sessions_.clear();
    }
    if (peer) peer->close();
    std::cerr << "Device registration revoked; active remote-control session terminated\n";
    return;
  }

  const std::string from = message.value("from", "");
  const std::string session = message.value("session", "");
  if (type == "auth-request") {
    if (from.empty() || session.empty()) return;
    HandleAuthRequest(from, session);
    return;
  }
  if (type == "auth-proof") {
    if (from.empty() || session.empty() || !message.contains("payload")) return;
    HandleAuthProof(from, session, message["payload"]);
    return;
  }
  if (type == "offer") {
    if (from.empty() || session.empty() || !message.contains("payload")) return;
    HandleOffer(from, session, message["payload"]);
    return;
  }

  if (type == "ice" && message.contains("payload")) {
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

void WebRtcSession::HandleAuthRequest(
    const std::string& from,
    const std::string& session) {
  if (config_.access_code.empty()) {
    std::cerr << "Rejected remote session: access code is not configured\n";
    SendSignalTo(
        from,
        session,
        "auth-rejected",
        json{{"reason", "host-unconfigured"}});
    return;
  }

  std::string nonce;
  if (!RandomNonceHex(&nonce)) {
    std::cerr << "Unable to generate access-code challenge\n";
    SendSignalTo(from, session, "auth-rejected", json{{"reason", "auth-unavailable"}});
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const std::string key = AccessSessionKey(from, session);
  bool capacity_available = true;
  {
    std::scoped_lock lock(mutex_);
    for (auto it = pending_access_challenges_.begin(); it != pending_access_challenges_.end();) {
      if (it->second.expires <= now) {
        it = pending_access_challenges_.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = authorized_offer_sessions_.begin(); it != authorized_offer_sessions_.end();) {
      if (it->second <= now) {
        it = authorized_offer_sessions_.erase(it);
      } else {
        ++it;
      }
    }

    if (!pending_access_challenges_.contains(key) &&
        pending_access_challenges_.size() >= kMaxPendingAccessChallenges) {
      capacity_available = false;
    } else {
      authorized_offer_sessions_.erase(key);
      pending_access_challenges_[key] = PendingAccessChallenge{
          nonce,
          now + kAccessChallengeLifetime,
      };
    }
  }

  if (!capacity_available) {
    SendSignalTo(from, session, "auth-rejected", json{{"reason", "auth-busy"}});
    return;
  }

  SendSignalTo(
      from,
      session,
      "auth-challenge",
      json{{"algorithm", kAccessProofAlgorithm}, {"nonce", nonce}, {"expiresInMs", 15000}});
}

void WebRtcSession::HandleAuthProof(
    const std::string& from,
    const std::string& session,
    const json& payload) {
  if (!payload.is_object()) return;
  const std::string proof = payload.value("proof", "");
  if (proof.size() != 64) {
    SendSignalTo(from, session, "auth-rejected", json{{"reason", "invalid-access-proof"}});
    return;
  }

  const std::string key = AccessSessionKey(from, session);
  const auto now = std::chrono::steady_clock::now();
  std::string nonce;
  bool challenge_valid = false;
  {
    std::scoped_lock lock(mutex_);
    const auto it = pending_access_challenges_.find(key);
    if (it != pending_access_challenges_.end()) {
      nonce = it->second.nonce;
      challenge_valid = it->second.expires > now;
      pending_access_challenges_.erase(it);
    }
  }

  if (!challenge_valid) {
    SendSignalTo(from, session, "auth-rejected", json{{"reason", "challenge-expired"}});
    return;
  }

  std::string expected_proof;
  const std::string proof_message = AccessProofMessage(
      from,
      config_.device_id,
      session,
      nonce);
  if (!HmacSha256Hex(config_.access_code, proof_message, &expected_proof)) {
    SendSignalTo(from, session, "auth-rejected", json{{"reason", "auth-unavailable"}});
    return;
  }

  if (!ConstantTimeEquals(proof, expected_proof)) {
    std::cerr << "Rejected remote session from " << from << ": invalid access proof\n";
    SendSignalTo(from, session, "auth-rejected", json{{"reason", "invalid-access-code"}});
    return;
  }

  {
    std::scoped_lock lock(mutex_);
    for (auto it = authorized_offer_sessions_.begin(); it != authorized_offer_sessions_.end();) {
      if (it->second <= now) {
        it = authorized_offer_sessions_.erase(it);
      } else {
        ++it;
      }
    }
    if (authorized_offer_sessions_.size() >= kMaxPendingAccessChallenges) {
      authorized_offer_sessions_.clear();
    }
    authorized_offer_sessions_[key] = now + kAccessChallengeLifetime;
  }

  std::cout << "Verified one-time access proof from " << from << "\n";
  SendSignalTo(from, session, "auth-accepted", json{{"version", 1}});
}

void WebRtcSession::HandleOffer(
    const std::string& from,
    const std::string& session,
    const json& payload) {
  if (!payload.is_object()) return;

  const std::string sdp = payload.value("sdp", "");
  const std::string description_type = payload.value("type", "offer");
  if (sdp.empty()) return;

  std::shared_ptr<rtc::PeerConnection> existing_peer;
  bool newly_authorized = false;
  const auto now = std::chrono::steady_clock::now();
  const std::string authorization_key = AccessSessionKey(from, session);
  {
    std::scoped_lock lock(mutex_);
    if (peer_ && from == controller_id_ && session == session_id_ &&
        peer_->state() != rtc::PeerConnection::State::Closed) {
      existing_peer = peer_;
    } else {
      const auto authorized = authorized_offer_sessions_.find(authorization_key);
      if (authorized != authorized_offer_sessions_.end()) {
        newly_authorized = authorized->second > now;
        authorized_offer_sessions_.erase(authorized);
      }
    }
  }

  if (!existing_peer && !newly_authorized) {
    std::cerr << "Rejected unauthenticated offer from " << from << "\n";
    SendSignalTo(from, session, "auth-rejected", json{{"reason", "auth-required"}});
    return;
  }

  const uint8_t h264_payload_type = FindH264PayloadType(sdp);
  if (h264_payload_type == 0) {
    std::cerr << "Controller offer does not contain H264; control will connect without video\n";
  } else {
    std::cout << "Negotiated H264 RTP payload type " << static_cast<int>(h264_payload_type) << "\n";
  }

  if (existing_peer) {
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

  std::string signal_auth_token;
  std::string signal_token_error;
  const bool signal_token_ready = ResolveSignalAuthToken(
      config_,
      &signal_auth_token,
      &signal_token_error);
  if (!signal_token_error.empty()) {
    std::cerr << signal_token_error
              << (signal_token_ready ? "; using configured signaling token fallback\n"
                                     : "; runtime token unavailable\n");
  }

  std::string turn_username = config_.turn_username;
  std::string turn_password = config_.turn_password;
  const std::string credential_endpoint = EnvString("DESKLINK_TURN_CREDENTIALS_URL");
  if (!credential_endpoint.empty()) {
    RuntimeTurnCredentials credentials;
    std::string credential_error;
    if (signal_token_ready && !signal_auth_token.empty() && FetchRuntimeTurnCredentials(
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
      if (credential_error.empty()) {
        credential_error = signal_token_ready
            ? "a signaling token is required for runtime TURN credentials"
            : "short-lived signaling token could not be refreshed";
      }
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
  std::weak_ptr<rtc::DataChannel> weak_channel = channel;
  channel->onOpen([this, label]() {
    std::cout << label << " DataChannel open\n";
    if (label == "control" && config_.on_monitor_state_requested) {
      config_.on_monitor_state_requested();
    }
  });
  channel->onClosed([this, label, weak_channel]() {
    if (label == "control") {
      input_.ReleaseAll();
      auto closed = weak_channel.lock();
      std::scoped_lock lock(mutex_);
      if (!closed || control_ == closed) control_.reset();
    }
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

  if (type == "video-profile") {
    if (!config_.on_video_profile_requested) return;

    const std::string mode = event.value("mode", "");
    if (mode == "auto") {
      config_.on_video_profile_requested(VideoProfile::Auto);
    } else if (mode == "original") {
      config_.on_video_profile_requested(VideoProfile::Original);
    } else if (mode == "high") {
      config_.on_video_profile_requested(VideoProfile::High);
    } else if (mode == "clear") {
      config_.on_video_profile_requested(VideoProfile::Clear);
    }
    return;
  }

  if (type == "monitor-list-request") {
    if (config_.on_monitor_state_requested) config_.on_monitor_state_requested();
    return;
  }

  if (type == "monitor-switch") {
    if (!config_.on_monitor_switch_requested) return;
    const auto index_it = event.find("index");
    if (index_it == event.end() || !index_it->is_number_integer()) return;
    const int64_t index = index_it->get<int64_t>();
    if (index < 0 || index > 63) return;
    input_.ReleaseAll();
    config_.on_monitor_switch_requested(static_cast<uint32_t>(index));
    return;
  }

  if (type == "clipboard-read-request") {
    const std::string request_id = event.value("requestId", "");
    if (!ValidRequestId(request_id)) return;

    std::string clipboard_text;
    std::string clipboard_error;
    if (ReadClipboardTextUtf8(&clipboard_text, &clipboard_error)) {
      SendControlMessage(json{
          {"t", "clipboard-text"},
          {"requestId", request_id},
          {"text", clipboard_text},
      }.dump());
    } else {
      SendControlMessage(json{
          {"t", "clipboard-result"},
          {"requestId", request_id},
          {"direction", "remote-to-local"},
          {"ok", false},
          {"error", clipboard_error},
      }.dump());
    }
    return;
  }

  if (type == "clipboard-write") {
    const std::string request_id = event.value("requestId", "");
    const auto text_it = event.find("text");
    if (!ValidRequestId(request_id) || text_it == event.end() || !text_it->is_string()) return;

    const std::string clipboard_text = text_it->get<std::string>();
    std::string clipboard_error;
    const bool ok = WriteClipboardTextUtf8(clipboard_text, &clipboard_error);
    json response = {
        {"t", "clipboard-result"},
        {"requestId", request_id},
        {"direction", "local-to-remote"},
        {"ok", ok},
    };
    if (!ok) response["error"] = clipboard_error;
    SendControlMessage(response.dump());
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
