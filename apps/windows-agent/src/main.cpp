#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include "desktop_capture.h"
#include "gpu_color_converter.h"
#include "h264_encoder.h"
#include "service_auth_client.h"
#include "video_policy.h"
#include "webrtc_session.h"

using Microsoft::WRL::ComPtr;

namespace {

std::atomic_bool g_running{true};
constexpr uint32_t kNoMonitorSwitch = std::numeric_limits<uint32_t>::max();

struct VideoSize {
  uint32_t width;
  uint32_t height;
};

struct VideoProfileLimits {
  uint32_t best_resolution_tier;
  uint32_t worst_resolution_tier;
  uint32_t max_fps;
  uint32_t max_bitrate_bps;
  const char* name;
};

BOOL WINAPI ConsoleHandler(DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
    g_running = false;
    return TRUE;
  }
  return FALSE;
}

uint64_t Now100ns() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() / 100);
}

std::string EnvOr(const char* name, std::string fallback) {
  char* value = nullptr;
  size_t length = 0;
  if (_dupenv_s(&value, &length, name) == 0 && value != nullptr) {
    std::string result(value);
    std::free(value);
    if (!result.empty()) return result;
    return fallback;
  }
  if (value) std::free(value);
  return fallback;
}

uint32_t EnvUIntOr(
    const char* name,
    uint32_t fallback,
    uint32_t minimum,
    uint32_t maximum) {
  const std::string value = EnvOr(name, "");
  if (value.empty()) return fallback;
  try {
    const unsigned long parsed = std::stoul(value);
    if (parsed >= minimum && parsed <= maximum) return static_cast<uint32_t>(parsed);
  } catch (...) {
  }
  return fallback;
}

uint16_t EnvPortOr(const char* name, uint16_t fallback) {
  return static_cast<uint16_t>(EnvUIntOr(name, fallback, 1, 65535));
}

std::string DefaultDeviceId() {
  char computer_name[MAX_COMPUTERNAME_LENGTH + 1]{};
  DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
  if (GetComputerNameA(computer_name, &size) && size > 0) {
    return std::string("win-") + computer_name;
  }
  return "windows-host";
}

VideoSize FitWithin(
    uint32_t source_width,
    uint32_t source_height,
    uint32_t max_width,
    uint32_t max_height) {
  const double scale = std::min({
      1.0,
      static_cast<double>(max_width) / source_width,
      static_cast<double>(max_height) / source_height,
  });

  uint32_t width = static_cast<uint32_t>(std::floor(source_width * scale));
  uint32_t height = static_cast<uint32_t>(std::floor(source_height * scale));
  width = std::max<uint32_t>(2, width & ~1U);
  height = std::max<uint32_t>(2, height & ~1U);
  return {width, height};
}

VideoSize ResolutionLimitForTier(
    uint32_t tier,
    uint32_t configured_width,
    uint32_t configured_height) {
  switch (std::min<uint32_t>(tier, 3)) {
    case 1:
      return {std::min<uint32_t>(configured_width, 1600),
              std::min<uint32_t>(configured_height, 900)};
    case 2:
      return {std::min<uint32_t>(configured_width, 1280),
              std::min<uint32_t>(configured_height, 720)};
    case 3:
      return {std::min<uint32_t>(configured_width, 960),
              std::min<uint32_t>(configured_height, 540)};
    default:
      return {configured_width, configured_height};
  }
}

uint32_t BitrateCapForResolutionTier(
    uint32_t target_bitrate,
    uint32_t min_bitrate,
    uint32_t tier) {
  static constexpr uint32_t kPercent[] = {100, 75, 50, 35};
  const uint32_t percent = kPercent[std::min<uint32_t>(tier, 3)];
  const uint64_t scaled = static_cast<uint64_t>(target_bitrate) * percent / 100;
  return std::clamp<uint32_t>(
      static_cast<uint32_t>(std::min<uint64_t>(scaled, target_bitrate)),
      min_bitrate,
      target_bitrate);
}

VideoProfileLimits LimitsForVideoProfile(
    desklink::VideoProfile profile,
    uint32_t target_fps,
    uint32_t target_bitrate,
    uint32_t min_bitrate) {
  auto bitrate_cap = [&](uint32_t preferred) {
    return std::max<uint32_t>(min_bitrate, std::min<uint32_t>(target_bitrate, preferred));
  };

  switch (profile) {
    case desklink::VideoProfile::Original:
      return {0, 0, target_fps, target_bitrate, "original"};
    case desklink::VideoProfile::High:
      return {1, 3, std::min<uint32_t>(target_fps, 45), bitrate_cap(8'000'000), "high"};
    case desklink::VideoProfile::Clear:
      return {2, 3, std::min<uint32_t>(target_fps, 30), bitrate_cap(4'000'000), "clear"};
    case desklink::VideoProfile::Auto:
    default:
      return {0, 3, target_fps, target_bitrate, "auto"};
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  uint32_t output_index = 0;
  if (argc > 1) {
    try {
      output_index = static_cast<uint32_t>(std::stoul(argv[1]));
    } catch (...) {
      std::wcerr << L"Usage: desklink-agent [output-index]\n";
      return 2;
    }
  }

  SetConsoleCtrlHandler(ConsoleHandler, TRUE);

  std::string resolved_access_code;
  if (desklink::ServiceAccessCodeBrokerConfigured()) {
    std::string access_error;
    if (!desklink::FetchServiceBrokerAccessCode(&resolved_access_code, &access_error)) {
      std::cerr << "SECURITY: protected access-code broker failed: " << access_error
                << "; refusing to start Agent\n";
      return 1;
    }
    std::cout << "Loaded unattended access code from LocalSystem Service broker.\n";
  } else {
    resolved_access_code = EnvOr("DESKLINK_ACCESS_CODE", "");
  }

  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(com)) {
    std::wcerr << L"CoInitializeEx failed: 0x" << std::hex << com << L"\n";
    return 1;
  }

  const HRESULT mf = MFStartup(MF_VERSION);
  if (FAILED(mf)) {
    std::wcerr << L"MFStartup failed: 0x" << std::hex << mf << L"\n";
    CoUninitialize();
    return 1;
  }

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

  D3D_FEATURE_LEVEL requested[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };
  D3D_FEATURE_LEVEL selected{};
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;

  HRESULT hr = D3D11CreateDevice(
      nullptr,
      D3D_DRIVER_TYPE_HARDWARE,
      nullptr,
      flags,
      requested,
      ARRAYSIZE(requested),
      D3D11_SDK_VERSION,
      &device,
      &selected,
      &context);

  if (FAILED(hr)) {
    std::wcerr << L"D3D11CreateDevice failed: 0x" << std::hex << hr << L"\n";
    MFShutdown();
    CoUninitialize();
    return 1;
  }

  ComPtr<IDXGIDevice> dxgi_device;
  hr = device.As(&dxgi_device);
  if (FAILED(hr)) {
    std::wcerr << L"IDXGIDevice query failed\n";
    MFShutdown();
    CoUninitialize();
    return 1;
  }

  ComPtr<IDXGIAdapter> adapter;
  dxgi_device->GetAdapter(&adapter);
  DXGI_ADAPTER_DESC adapter_desc{};
  adapter->GetDesc(&adapter_desc);

  desklink::DesktopCapture capture;
  if (!capture.Initialize(device.Get(), output_index)) {
    std::wcerr << L"Unable to initialize desktop capture for output " << output_index << L"\n";
    MFShutdown();
    CoUninitialize();
    return 1;
  }

  uint32_t target_fps = EnvUIntOr("DESKLINK_FPS", 60, 15, 144);
  const uint32_t target_bitrate = EnvUIntOr(
      "DESKLINK_BITRATE_BPS",
      desklink::DefaultBitrateForFps(target_fps),
      1'000'000,
      60'000'000);
  const uint32_t min_bitrate = EnvUIntOr(
      "DESKLINK_MIN_BITRATE_BPS",
      std::min<uint32_t>(2'000'000, target_bitrate),
      500'000,
      target_bitrate);
  const uint32_t max_width = EnvUIntOr("DESKLINK_MAX_WIDTH", 1920, 640, 3840);
  const uint32_t max_height = EnvUIntOr("DESKLINK_MAX_HEIGHT", 1080, 360, 2160);

  uint32_t source_width = capture.width();
  uint32_t source_height = capture.height();
  VideoSize encode_size = FitWithin(source_width, source_height, max_width, max_height);

  long controlled_left = capture.left();
  long controlled_top = capture.top();
  long controlled_width = static_cast<long>(capture.width());
  long controlled_height = static_cast<long>(capture.height());

  desklink::GpuColorConverter converter;
  desklink::H264Encoder encoder;
  bool converter_ready = false;
  bool encoder_ready = false;

  auto initialize_video_pipeline_at_fps = [&](uint32_t fps) {
    encoder.Reset();
    converter.Reset();
    converter_ready = converter.Initialize(
        device.Get(),
        source_width,
        source_height,
        encode_size.width,
        encode_size.height,
        fps);
    encoder_ready = converter_ready && encoder.Initialize(
        device.Get(),
        encode_size.width,
        encode_size.height,
        fps,
        target_bitrate);
    return encoder_ready;
  };

  const uint32_t requested_target_fps = target_fps;
  initialize_video_pipeline_at_fps(target_fps);
  while (!encoder_ready && target_fps > 60) {
    const uint32_t fallback_fps = desklink::LowerFpsTier(target_fps, target_fps);
    if (fallback_fps >= target_fps) break;
    std::wcerr << L"Video pipeline unavailable at " << target_fps
               << L" fps; retrying " << fallback_fps << L" fps.\n";
    target_fps = fallback_fps;
    initialize_video_pipeline_at_fps(target_fps);
  }
  if (encoder_ready && target_fps != requested_target_fps) {
    std::wcout << L"High-refresh compatibility fallback selected " << target_fps
               << L" fps instead of requested " << requested_target_fps << L" fps.\n";
  }

  if (!converter_ready) {
    std::wcerr << L"GPU BGRA->NV12 conversion unavailable; remote input remains available but video is disabled.\n";
  } else if (!encoder_ready) {
    std::wcerr << L"Hardware H264 encoder unavailable; remote input remains available but video is disabled.\n";
  }

  std::atomic<uint32_t> requested_bitrate{target_bitrate};
  std::atomic<uint32_t> requested_fps{target_fps};
  std::atomic<uint32_t> requested_resolution_tier{0};
  std::atomic<uint32_t> profile_max_bitrate{target_bitrate};
  std::atomic<uint32_t> profile_max_fps{target_fps};
  std::atomic<uint32_t> profile_best_resolution_tier{0};
  std::atomic<uint32_t> profile_worst_resolution_tier{3};
  std::atomic<uint32_t> requested_output_index{kNoMonitorSwitch};
  std::atomic_bool monitor_state_requested{false};
  std::atomic_bool keyframe_requested{false};
  std::mutex adaptation_mutex;
  auto last_bitrate_change = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  auto last_fps_change = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  auto last_resolution_change = std::chrono::steady_clock::now() - std::chrono::seconds(20);
  uint32_t severe_feedback_streak = 0;
  uint32_t healthy_feedback_streak = 0;
  uint32_t resolution_pressure_streak = 0;
  uint32_t resolution_healthy_streak = 0;

  desklink::SessionConfig session_config;
  session_config.signal_url = EnvOr("DESKLINK_SIGNAL_URL", session_config.signal_url);
  session_config.device_id = EnvOr("DESKLINK_DEVICE_ID", DefaultDeviceId());
  session_config.access_code = std::move(resolved_access_code);
  session_config.stun_url = EnvOr("DESKLINK_STUN_URL", session_config.stun_url);
  session_config.turn_host = EnvOr("DESKLINK_TURN_HOST", session_config.turn_host);
  session_config.turn_port = EnvPortOr("DESKLINK_TURN_PORT", session_config.turn_port);
  session_config.turn_username = EnvOr("DESKLINK_TURN_USERNAME", session_config.turn_username);
  session_config.turn_password = EnvOr("DESKLINK_TURN_PASSWORD", session_config.turn_password);
  session_config.media_pacing_bitrate_bps = desklink::PacingBitrateForMedia(target_bitrate);
  session_config.media_pacing_interval_ms = desklink::PacingIntervalMsForFps(target_fps);
  session_config.on_keyframe_requested = [&]() {
    keyframe_requested.store(true, std::memory_order_relaxed);
  };
  session_config.on_monitor_state_requested = [&]() {
    monitor_state_requested.store(true, std::memory_order_relaxed);
  };
  session_config.on_monitor_switch_requested = [&](uint32_t index) {
    requested_output_index.store(index, std::memory_order_relaxed);
  };
  session_config.on_video_profile_requested = [&](desklink::VideoProfile profile) {
    std::scoped_lock lock(adaptation_mutex);
    const VideoProfileLimits limits = LimitsForVideoProfile(
        profile,
        target_fps,
        target_bitrate,
        min_bitrate);

    profile_max_bitrate.store(limits.max_bitrate_bps, std::memory_order_relaxed);
    profile_max_fps.store(limits.max_fps, std::memory_order_relaxed);
    profile_best_resolution_tier.store(limits.best_resolution_tier, std::memory_order_relaxed);
    profile_worst_resolution_tier.store(limits.worst_resolution_tier, std::memory_order_relaxed);
    requested_bitrate.store(limits.max_bitrate_bps, std::memory_order_relaxed);
    requested_fps.store(limits.max_fps, std::memory_order_relaxed);
    requested_resolution_tier.store(limits.best_resolution_tier, std::memory_order_relaxed);

    severe_feedback_streak = 0;
    healthy_feedback_streak = 0;
    resolution_pressure_streak = 0;
    resolution_healthy_streak = 0;
    const auto now = std::chrono::steady_clock::now();
    last_bitrate_change = now;
    last_fps_change = now;
    last_resolution_change = now;
    keyframe_requested.store(true, std::memory_order_relaxed);

    std::cout << "Controller video profile: " << limits.name
              << ", fps<= " << limits.max_fps
              << ", bitrate<= " << (limits.max_bitrate_bps / 1'000'000.0)
              << " Mbps, resolution-tier " << limits.best_resolution_tier
              << ".." << limits.worst_resolution_tier << "\n";
  };
  session_config.on_network_feedback = [&](const desklink::NetworkFeedback& feedback) {
    std::scoped_lock lock(adaptation_mutex);
    const auto now = std::chrono::steady_clock::now();
    const auto since_change = now - last_bitrate_change;
    const uint32_t bitrate_ceiling = profile_max_bitrate.load(std::memory_order_relaxed);
    const uint32_t fps_ceiling = profile_max_fps.load(std::memory_order_relaxed);
    const uint32_t best_resolution_tier =
        profile_best_resolution_tier.load(std::memory_order_relaxed);
    const uint32_t worst_resolution_tier =
        profile_worst_resolution_tier.load(std::memory_order_relaxed);
    const uint32_t current = std::min<uint32_t>(
        requested_bitrate.load(std::memory_order_relaxed),
        bitrate_ceiling);
    uint32_t next = current;

    const bool capacity_severe = feedback.available_incoming_bitrate_bps > 0.0 &&
                                 feedback.available_incoming_bitrate_bps <
                                     static_cast<double>(current) * 0.70;
    const bool capacity_moderate = feedback.available_incoming_bitrate_bps > 0.0 &&
                                   feedback.available_incoming_bitrate_bps <
                                       static_cast<double>(current) * 0.95;
    const bool capacity_healthy = feedback.available_incoming_bitrate_bps <= 0.0 ||
                                  feedback.available_incoming_bitrate_bps >
                                      static_cast<double>(current) * 1.25;

    const bool severe = feedback.loss_ratio >= 0.08 ||
                        feedback.rtt_ms >= 250.0 ||
                        feedback.jitter_ms >= 80.0 ||
                        capacity_severe;
    const bool moderate = feedback.loss_ratio >= 0.03 ||
                          feedback.rtt_ms >= 160.0 ||
                          feedback.jitter_ms >= 45.0 ||
                          capacity_moderate;
    const bool healthy = feedback.loss_ratio < 0.01 &&
                         (feedback.rtt_ms <= 0.0 || feedback.rtt_ms < 100.0) &&
                         feedback.jitter_ms < 30.0 &&
                         capacity_healthy;

    if (severe && since_change >= std::chrono::seconds(1)) {
      next = std::max<uint32_t>(min_bitrate, static_cast<uint32_t>(current * 0.65));
    } else if (moderate && since_change >= std::chrono::seconds(2)) {
      next = std::max<uint32_t>(min_bitrate, static_cast<uint32_t>(current * 0.82));
    } else if (healthy && since_change >= std::chrono::seconds(5)) {
      const uint32_t increase = std::max<uint32_t>(250'000, current / 12);
      next = std::min<uint32_t>(bitrate_ceiling, current + increase);
    }

    if (feedback.available_incoming_bitrate_bps > 0.0 &&
        feedback.available_incoming_bitrate_bps < static_cast<double>(current) * 1.05) {
      const uint32_t capacity_target = std::max<uint32_t>(
          min_bitrate,
          static_cast<uint32_t>(feedback.available_incoming_bitrate_bps * 0.82));
      next = std::min(next, capacity_target);
    }

    next = std::clamp(next, min_bitrate, bitrate_ceiling);
    if (next != current) {
      requested_bitrate.store(next, std::memory_order_relaxed);
      last_bitrate_change = now;
      if (next < current && feedback.loss_ratio >= 0.15) {
        keyframe_requested.store(true, std::memory_order_relaxed);
      }
    }

    if (severe) {
      severe_feedback_streak = std::min<uint32_t>(severe_feedback_streak + 1, 60);
      healthy_feedback_streak = 0;
    } else if (healthy) {
      healthy_feedback_streak = std::min<uint32_t>(healthy_feedback_streak + 1, 60);
      severe_feedback_streak = 0;
    } else {
      severe_feedback_streak = 0;
      healthy_feedback_streak = 0;
    }

    const uint32_t current_fps = std::min<uint32_t>(
        requested_fps.load(std::memory_order_relaxed),
        fps_ceiling);
    if (severe_feedback_streak >= 3 &&
        now - last_fps_change >= std::chrono::seconds(3)) {
      const uint32_t lower = desklink::LowerFpsTier(current_fps, fps_ceiling);
      if (lower < current_fps) {
        requested_fps.store(lower, std::memory_order_relaxed);
        last_fps_change = now;
      }
      severe_feedback_streak = 0;
    } else if (healthy_feedback_streak >= 8 &&
               now - last_fps_change >= std::chrono::seconds(8)) {
      const uint32_t higher = desklink::RaiseFpsTier(current_fps, fps_ceiling);
      if (higher > current_fps) {
        requested_fps.store(higher, std::memory_order_relaxed);
        last_fps_change = now;
      }
      healthy_feedback_streak = 0;
    }

    const uint32_t adaptive_fps = requested_fps.load(std::memory_order_relaxed);
    const uint32_t fps_floor = std::min<uint32_t>(fps_ceiling, 24);
    const uint32_t bitrate_pressure_floor = std::max<uint32_t>(
        min_bitrate,
        static_cast<uint32_t>(static_cast<uint64_t>(bitrate_ceiling) * 45 / 100));
    const bool resolution_exhausted = adaptive_fps <= fps_floor &&
                                      next <= bitrate_pressure_floor;

    if (severe && resolution_exhausted) {
      resolution_pressure_streak = std::min<uint32_t>(resolution_pressure_streak + 1, 60);
    } else {
      resolution_pressure_streak = 0;
    }

    const uint32_t recovery_fps = std::min<uint32_t>(fps_ceiling, 45);
    const uint32_t recovery_bitrate = std::max<uint32_t>(
        min_bitrate,
        static_cast<uint32_t>(static_cast<uint64_t>(bitrate_ceiling) * 60 / 100));
    if (healthy && adaptive_fps >= recovery_fps && next >= recovery_bitrate) {
      resolution_healthy_streak = std::min<uint32_t>(resolution_healthy_streak + 1, 120);
    } else {
      resolution_healthy_streak = 0;
    }

    const uint32_t current_tier = std::clamp<uint32_t>(
        requested_resolution_tier.load(std::memory_order_relaxed),
        best_resolution_tier,
        worst_resolution_tier);
    if (resolution_pressure_streak >= 4 &&
        now - last_resolution_change >= std::chrono::seconds(8) &&
        current_tier < worst_resolution_tier) {
      requested_resolution_tier.store(current_tier + 1, std::memory_order_relaxed);
      last_resolution_change = now;
      resolution_pressure_streak = 0;
    } else if (resolution_healthy_streak >= 12 &&
               now - last_resolution_change >= std::chrono::seconds(15) &&
               current_tier > best_resolution_tier) {
      requested_resolution_tier.store(current_tier - 1, std::memory_order_relaxed);
      last_resolution_change = now;
      resolution_healthy_streak = 0;
    }
  };

  if (session_config.access_code.empty()) {
    std::wcerr << L"SECURITY: no access code is configured; incoming remote-control offers will be rejected.\n";
  }

  rtc::InitLogger(rtc::LogLevel::Info);
  desklink::WebRtcSession session(std::move(session_config));
  session.SetControlledDesktopRect(
      controlled_left,
      controlled_top,
      controlled_width,
      controlled_height);
  session.Start();

  std::wcout << L"DeskLink Windows Agent\n"
             << L"GPU: " << adapter_desc.Description << L"\n"
             << L"Desktop: output " << capture.output_index() << L" ("
             << source_width << L"x" << source_height << L", origin "
             << controlled_left << L"," << controlled_top << L")\n"
             << L"Stream target: " << encode_size.width << L"x" << encode_size.height
             << L" @ up to " << target_fps << L" fps, "
             << std::fixed << std::setprecision(1)
             << (target_bitrate / 1'000'000.0) << L" Mbps max, "
             << (min_bitrate / 1'000'000.0) << L" Mbps min\n"
             << L"Press Ctrl+C to stop.\n";

  using clock = std::chrono::steady_clock;
  auto window_start = clock::now();
  uint64_t captured_frames = 0;
  uint64_t encoded_frames = 0;
  uint64_t sent_frames = 0;
  uint64_t encoded_bytes = 0;
  uint64_t timeout_ticks = 0;
  uint64_t last_encoded_timestamp100ns = 0;
  uint64_t last_fresh_encode_timestamp100ns = 0;
  uint64_t last_cache_update_timestamp100ns = 0;
  uint32_t active_bitrate = target_bitrate;
  uint32_t active_fps = target_fps;
  uint32_t active_resolution_tier = 0;
  bool was_connected = false;
  auto last_cached_recovery = clock::now() - std::chrono::seconds(1);

  auto apply_capture_geometry = [&]() {
    source_width = capture.width();
    source_height = capture.height();
    controlled_left = capture.left();
    controlled_top = capture.top();
    controlled_width = static_cast<long>(capture.width());
    controlled_height = static_cast<long>(capture.height());
    session.SetControlledDesktopRect(
        controlled_left,
        controlled_top,
        controlled_width,
        controlled_height);
  };

  auto send_monitor_state = [&]() {
    desklink::ServiceSecureAttentionStatus sas_status;
    std::string sas_error;
    const bool sas_status_ok = desklink::FetchServiceSecureAttentionStatus(
        &sas_status,
        &sas_error);
    std::string sas_reason;
    if (!sas_status_ok) {
      sas_reason = "capability-unavailable";
    } else if (!sas_status.broker_configured) {
      sas_reason = "service-broker-unavailable";
    } else if (!sas_status.api_available) {
      sas_reason = "api-unavailable";
    } else if (!sas_status.policy_readable) {
      sas_reason = "policy-read-error";
    } else if (!sas_status.policy_allows_services) {
      sas_reason = "policy-not-allowed";
    }

    session.SendControlMessage(nlohmann::json{
        {"t", "host-capabilities"},
        {"version", 1},
        {"secureAttentionAvailable", sas_status_ok && sas_status.available},
        {"secureAttentionReason", sas_reason},
        {"secureAttentionPolicy", sas_status.policy},
        {"clipboardAvailable", true},
        {"fileTransferAvailable", true},
        {"audioAvailable", false},
        {"protectedDesktopAvailable", false},
    }.dump());

    nlohmann::json monitors = nlohmann::json::array();
    for (const auto& display : capture.EnumerateOutputs()) {
      monitors.push_back({
          {"index", display.index},
          {"name", display.name},
          {"left", display.left},
          {"top", display.top},
          {"width", display.width},
          {"height", display.height},
          {"primary", display.primary},
      });
    }
    session.SendControlMessage(nlohmann::json{
        {"t", "monitor-state"},
        {"activeIndex", capture.output_index()},
        {"monitors", std::move(monitors)},
    }.dump());
  };

  auto encode_and_send = [&](ID3D11Texture2D* nv12, uint64_t timestamp100ns) -> bool {
    if (!nv12 || !encoder_ready) return false;

    if (timestamp100ns <= last_encoded_timestamp100ns) {
      timestamp100ns = last_encoded_timestamp100ns + 1;
    }

    desklink::EncodedH264Frame encoded;
    if (!encoder.Encode(nv12, timestamp100ns, &encoded)) return false;

    last_encoded_timestamp100ns = timestamp100ns;
    ++encoded_frames;
    encoded_bytes += encoded.bytes.size();
    if (session.SendH264AccessUnit(
            encoded.bytes.data(),
            encoded.bytes.size(),
            encoded.timestamp100ns)) {
      ++sent_frames;
      return true;
    }

    keyframe_requested.store(true, std::memory_order_relaxed);
    return false;
  };

  auto rebuild_video_pipeline = [&](uint32_t tier, const wchar_t* reason, bool force) -> bool {
    const uint32_t best_tier = profile_best_resolution_tier.load(std::memory_order_relaxed);
    const uint32_t worst_tier = profile_worst_resolution_tier.load(std::memory_order_relaxed);
    tier = std::clamp<uint32_t>(tier, best_tier, worst_tier);
    const VideoSize limit = ResolutionLimitForTier(tier, max_width, max_height);
    const VideoSize next_size = FitWithin(
        source_width,
        source_height,
        limit.width,
        limit.height);

    if (!force && encoder_ready && converter_ready &&
        next_size.width == encode_size.width && next_size.height == encode_size.height) {
      active_resolution_tier = tier;
      return true;
    }

    const uint32_t profile_bitrate_ceiling =
        profile_max_bitrate.load(std::memory_order_relaxed);
    const uint32_t bitrate_cap = std::min<uint32_t>(
        profile_bitrate_ceiling,
        BitrateCapForResolutionTier(target_bitrate, min_bitrate, tier));
    const uint32_t restart_bitrate = std::min<uint32_t>(
        std::clamp(
            requested_bitrate.load(std::memory_order_relaxed),
            min_bitrate,
            profile_bitrate_ceiling),
        bitrate_cap);

    encoder.Reset();
    converter.Reset();
    converter_ready = converter.Initialize(
        device.Get(),
        source_width,
        source_height,
        next_size.width,
        next_size.height,
        target_fps);
    encoder_ready = converter_ready && encoder.Initialize(
        device.Get(),
        next_size.width,
        next_size.height,
        target_fps,
        restart_bitrate);

    if (!encoder_ready) return false;

    encode_size = next_size;
    active_bitrate = restart_bitrate;
    active_resolution_tier = tier;
    last_encoded_timestamp100ns = 0;
    last_fresh_encode_timestamp100ns = 0;
    last_cache_update_timestamp100ns = 0;
    keyframe_requested.store(true, std::memory_order_relaxed);

    std::wcout << L"\n" << reason << L": stream "
               << encode_size.width << L"x" << encode_size.height
               << L", resolution-tier=" << active_resolution_tier
               << L", bitrate-cap="
               << (bitrate_cap / 1'000'000.0) << L" Mbps\n";
    return true;
  };

  while (g_running) {
    const bool connected = session.connected();
    if (connected && !was_connected && encoder_ready) {
      keyframe_requested.store(true, std::memory_order_relaxed);
    } else if (!connected && was_connected) {
      const VideoProfileLimits automatic = LimitsForVideoProfile(
          desklink::VideoProfile::Auto,
          target_fps,
          target_bitrate,
          min_bitrate);
      profile_max_bitrate.store(automatic.max_bitrate_bps, std::memory_order_relaxed);
      profile_max_fps.store(automatic.max_fps, std::memory_order_relaxed);
      profile_best_resolution_tier.store(automatic.best_resolution_tier, std::memory_order_relaxed);
      profile_worst_resolution_tier.store(automatic.worst_resolution_tier, std::memory_order_relaxed);
      requested_fps.store(automatic.max_fps, std::memory_order_relaxed);
      requested_resolution_tier.store(automatic.best_resolution_tier, std::memory_order_relaxed);
      requested_bitrate.store(automatic.max_bitrate_bps, std::memory_order_relaxed);
      requested_output_index.store(kNoMonitorSwitch, std::memory_order_relaxed);
      monitor_state_requested.store(false, std::memory_order_relaxed);
      if (encoder_ready && active_bitrate != automatic.max_bitrate_bps) {
        if (encoder.SetBitrate(automatic.max_bitrate_bps)) {
          active_bitrate = automatic.max_bitrate_bps;
        }
      }
    }
    was_connected = connected;

    if (monitor_state_requested.exchange(false, std::memory_order_relaxed)) {
      send_monitor_state();
    }

    const uint32_t requested_output =
        requested_output_index.exchange(kNoMonitorSwitch, std::memory_order_relaxed);
    if (requested_output != kNoMonitorSwitch) {
      const uint32_t previous_output = capture.output_index();
      bool switch_ok = true;
      std::string switch_error;

      if (requested_output != previous_output) {
        if (!capture.SwitchOutput(requested_output)) {
          switch_ok = false;
          switch_error = "monitor-unavailable";
        } else {
          apply_capture_geometry();
          const uint32_t previous_tier = active_resolution_tier;
          if (!rebuild_video_pipeline(previous_tier, L"Monitor switched", true)) {
            switch_ok = false;
            switch_error = "video-pipeline-rebuild-failed";
            if (capture.SwitchOutput(previous_output)) {
              apply_capture_geometry();
              if (!rebuild_video_pipeline(previous_tier, L"Monitor switch rollback", true)) {
                switch_error = "video-pipeline-rollback-failed";
              }
            } else {
              switch_error = "monitor-rollback-failed";
            }
          }
        }
      }

      nlohmann::json switch_result = {
          {"t", "monitor-switch-result"},
          {"index", requested_output},
          {"ok", switch_ok},
          {"activeIndex", capture.output_index()},
      };
      if (!switch_ok) switch_result["error"] = switch_error;
      session.SendControlMessage(switch_result.dump());
      send_monitor_state();
    }

    const uint32_t fps_ceiling = connected
        ? profile_max_fps.load(std::memory_order_relaxed)
        : target_fps;
    const uint32_t desired_fps = connected
        ? std::min<uint32_t>(requested_fps.load(std::memory_order_relaxed), fps_ceiling)
        : target_fps;
    if (desired_fps != active_fps) {
      active_fps = std::clamp<uint32_t>(desired_fps, 15, target_fps);
      if (!connected) requested_fps.store(target_fps, std::memory_order_relaxed);
      std::wcout << L"\nAdaptive frame-rate target: " << active_fps << L" fps\n";
    }

    if (connected) {
      const uint32_t best_tier = profile_best_resolution_tier.load(std::memory_order_relaxed);
      const uint32_t worst_tier = profile_worst_resolution_tier.load(std::memory_order_relaxed);
      const uint32_t desired_resolution_tier = std::clamp<uint32_t>(
          requested_resolution_tier.load(std::memory_order_relaxed),
          best_tier,
          worst_tier);
      if (desired_resolution_tier != active_resolution_tier) {
        const uint32_t previous_tier = active_resolution_tier;
        if (!rebuild_video_pipeline(
                desired_resolution_tier,
                L"Video profile/adaptive resolution changed",
                false)) {
          std::wcerr << L"\nAdaptive resolution rebuild failed; restoring previous profile.\n";
          if (!rebuild_video_pipeline(previous_tier, L"Restored previous profile", true)) {
            std::wcerr << L"Video pipeline restore failed; remote input remains active.\n";
          }
          requested_resolution_tier.store(active_resolution_tier, std::memory_order_relaxed);
        }
      }
    }

    if (connected && encoder_ready) {
      const uint32_t profile_bitrate_ceiling =
          profile_max_bitrate.load(std::memory_order_relaxed);
      const uint32_t bitrate_cap = std::min<uint32_t>(
          profile_bitrate_ceiling,
          BitrateCapForResolutionTier(
              target_bitrate,
              min_bitrate,
              active_resolution_tier));
      const uint32_t desired_bitrate = std::min<uint32_t>(
          requested_bitrate.load(std::memory_order_relaxed),
          bitrate_cap);
      if (desired_bitrate != active_bitrate) {
        if (encoder.SetBitrate(desired_bitrate)) {
          active_bitrate = desired_bitrate;
        } else {
          requested_bitrate.store(active_bitrate, std::memory_order_relaxed);
        }
      }
    }

    bool encoded_fresh_frame = false;
    auto frame = capture.Acquire(desklink::CaptureTimeoutMsForFps(active_fps));

    const long new_controlled_left = capture.left();
    const long new_controlled_top = capture.top();
    const long new_controlled_width = static_cast<long>(capture.width());
    const long new_controlled_height = static_cast<long>(capture.height());
    if (new_controlled_left != controlled_left ||
        new_controlled_top != controlled_top ||
        new_controlled_width != controlled_width ||
        new_controlled_height != controlled_height) {
      controlled_left = new_controlled_left;
      controlled_top = new_controlled_top;
      controlled_width = new_controlled_width;
      controlled_height = new_controlled_height;
      session.SetControlledDesktopRect(
          controlled_left,
          controlled_top,
          controlled_width,
          controlled_height);
      std::wcout << L"\nControlled monitor moved/resized to "
                 << controlled_left << L"," << controlled_top << L" "
                 << controlled_width << L"x" << controlled_height << L"\n";
      monitor_state_requested.store(true, std::memory_order_relaxed);
    }

    if (frame) {
      ++captured_frames;

      if (frame->width != source_width || frame->height != source_height) {
        source_width = frame->width;
        source_height = frame->height;
        const uint32_t previous_tier = active_resolution_tier;
        if (!rebuild_video_pipeline(previous_tier, L"Display changed", true)) {
          std::wcerr << L"\nDisplay changed but the current video profile could not be rebuilt.\n";
          const uint32_t profile_best_tier =
              profile_best_resolution_tier.load(std::memory_order_relaxed);
          if (previous_tier != profile_best_tier &&
              rebuild_video_pipeline(profile_best_tier, L"Fallback profile resolution", true)) {
            requested_resolution_tier.store(profile_best_tier, std::memory_order_relaxed);
          } else if (!encoder_ready) {
            std::wcerr << L"Video disabled after display change; remote input remains active.\n";
          }
        }
        monitor_state_requested.store(true, std::memory_order_relaxed);
      }

      const bool recovery_requested = keyframe_requested.load(std::memory_order_relaxed);
      const uint64_t min_frame_interval100ns =
          10'000'000ULL / std::max<uint32_t>(1, active_fps);
      const bool fresh_frame_due = last_fresh_encode_timestamp100ns == 0 ||
                                   frame->timestamp100ns >=
                                       last_fresh_encode_timestamp100ns + min_frame_interval100ns;

      const uint64_t idle_cache_interval100ns = 1'000'000ULL;
      const bool cache_update_due = connected
          ? (fresh_frame_due || recovery_requested)
          : (last_cache_update_timestamp100ns == 0 ||
             frame->timestamp100ns >=
                 last_cache_update_timestamp100ns + idle_cache_interval100ns);

      ComPtr<ID3D11Texture2D> nv12;
      if (converter_ready && cache_update_due) {
        nv12 = converter.Convert(frame->texture.Get());
        if (nv12) last_cache_update_timestamp100ns = frame->timestamp100ns;
      }

      if (connected && encoder_ready && nv12 &&
          (fresh_frame_due || recovery_requested)) {
        if (keyframe_requested.exchange(false, std::memory_order_relaxed)) {
          encoder.RequestKeyframe();
        }
        encoded_fresh_frame = encode_and_send(nv12.Get(), frame->timestamp100ns);
        if (encoded_fresh_frame) {
          last_fresh_encode_timestamp100ns = frame->timestamp100ns;
        }
      }
    } else {
      ++timeout_ticks;
    }

    if (connected && encoder_ready && !encoded_fresh_frame &&
        keyframe_requested.load(std::memory_order_relaxed)) {
      const auto now = clock::now();
      if (now - last_cached_recovery >= std::chrono::milliseconds(250)) {
        auto cached = converter.LatestFrame();
        if (cached) {
          keyframe_requested.store(false, std::memory_order_relaxed);
          encoder.RequestKeyframe();
          last_cached_recovery = now;
          if (!encode_and_send(cached.Get(), Now100ns())) {
            keyframe_requested.store(true, std::memory_order_relaxed);
          }
        }
      }
    }

    const auto now = clock::now();
    const auto elapsed = std::chrono::duration<double>(now - window_start).count();
    if (elapsed >= 1.0) {
      const double capture_fps = static_cast<double>(captured_frames) / elapsed;
      const double encode_fps = static_cast<double>(encoded_frames) / elapsed;
      const double media_mbps = static_cast<double>(encoded_bytes) * 8.0 / elapsed / 1'000'000.0;
      std::wcout << L"capture=" << std::fixed << std::setprecision(1) << capture_fps
                 << L" fps, encode=" << encode_fps
                 << L" fps, sent=" << sent_frames
                 << L", media=" << media_mbps << L" Mbps"
                 << L", target=" << (active_bitrate / 1'000'000.0) << L" Mbps"
                 << L", fps-target=" << active_fps
                 << L", size=" << encode_size.width << L"x" << encode_size.height
                 << L", tier=" << active_resolution_tier
                 << L", output=" << capture.output_index()
                 << L", idle=" << timeout_ticks
                 << (connected ? L", controller=connected" : L", controller=waiting")
                 << L"        \r" << std::flush;
      captured_frames = 0;
      encoded_frames = 0;
      sent_frames = 0;
      encoded_bytes = 0;
      timeout_ticks = 0;
      window_start = now;
    }
  }

  std::wcout << L"\nDeskLink Agent stopped.\n";
  session.Stop();
  encoder.Reset();
  converter.Reset();
  capture.Reset();
  context->ClearState();
  context->Flush();
  MFShutdown();
  CoUninitialize();
  return 0;
}
