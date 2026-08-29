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
#include <mutex>
#include <string>

#include <rtc/rtc.hpp>

#include "desktop_capture.h"
#include "gpu_color_converter.h"
#include "h264_encoder.h"
#include "webrtc_session.h"

using Microsoft::WRL::ComPtr;

namespace {

std::atomic_bool g_running{true};

struct VideoSize {
  uint32_t width;
  uint32_t height;
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

uint32_t LowerFpsTier(uint32_t current, uint32_t target) {
  if (target <= 15 || current <= 15) return std::min(current, target);
  if (current > 45 && target > 45) return 45;
  if (current > 30 && target > 30) return 30;
  if (current > 24 && target > 24) return 24;
  return std::max<uint32_t>(15, std::min(current, target));
}

uint32_t RaiseFpsTier(uint32_t current, uint32_t target) {
  if (current >= target) return target;
  if (current < 30 && target > current) return std::min<uint32_t>(target, 30);
  if (current < 45 && target > current) return std::min<uint32_t>(target, 45);
  return target;
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

  const uint32_t target_fps = EnvUIntOr("DESKLINK_FPS", 60, 15, 60);
  const uint32_t target_bitrate = EnvUIntOr(
      "DESKLINK_BITRATE_BPS",
      12'000'000,
      1'000'000,
      50'000'000);
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
  bool converter_ready = converter.Initialize(
      device.Get(),
      source_width,
      source_height,
      encode_size.width,
      encode_size.height,
      target_fps);

  desklink::H264Encoder encoder;
  bool encoder_ready = converter_ready && encoder.Initialize(
      device.Get(),
      encode_size.width,
      encode_size.height,
      target_fps,
      target_bitrate);

  if (!converter_ready) {
    std::wcerr << L"GPU BGRA->NV12 conversion unavailable; remote input remains available but video is disabled.\n";
  } else if (!encoder_ready) {
    std::wcerr << L"Hardware H264 encoder unavailable; remote input remains available but video is disabled.\n";
  }

  std::atomic<uint32_t> requested_bitrate{target_bitrate};
  std::atomic<uint32_t> requested_fps{target_fps};
  std::atomic_bool keyframe_requested{false};
  std::mutex adaptation_mutex;
  auto last_bitrate_change = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  auto last_fps_change = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  uint32_t severe_feedback_streak = 0;
  uint32_t healthy_feedback_streak = 0;

  desklink::SessionConfig session_config;
  session_config.signal_url = EnvOr("DESKLINK_SIGNAL_URL", session_config.signal_url);
  session_config.device_id = EnvOr("DESKLINK_DEVICE_ID", DefaultDeviceId());
  session_config.access_code = EnvOr("DESKLINK_ACCESS_CODE", "");
  session_config.stun_url = EnvOr("DESKLINK_STUN_URL", session_config.stun_url);
  session_config.turn_host = EnvOr("DESKLINK_TURN_HOST", session_config.turn_host);
  session_config.turn_port = EnvPortOr("DESKLINK_TURN_PORT", session_config.turn_port);
  session_config.turn_username = EnvOr("DESKLINK_TURN_USERNAME", session_config.turn_username);
  session_config.turn_password = EnvOr("DESKLINK_TURN_PASSWORD", session_config.turn_password);
  session_config.on_keyframe_requested = [&]() {
    keyframe_requested.store(true, std::memory_order_relaxed);
  };
  session_config.on_network_feedback = [&](const desklink::NetworkFeedback& feedback) {
    std::scoped_lock lock(adaptation_mutex);
    const auto now = std::chrono::steady_clock::now();
    const auto since_change = now - last_bitrate_change;
    const uint32_t current = requested_bitrate.load(std::memory_order_relaxed);
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
      next = std::min<uint32_t>(target_bitrate, current + increase);
    }

    if (feedback.available_incoming_bitrate_bps > 0.0 &&
        feedback.available_incoming_bitrate_bps < static_cast<double>(current) * 1.05) {
      const uint32_t capacity_target = std::max<uint32_t>(
          min_bitrate,
          static_cast<uint32_t>(feedback.available_incoming_bitrate_bps * 0.82));
      next = std::min(next, capacity_target);
    }

    next = std::clamp(next, min_bitrate, target_bitrate);
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

    const uint32_t current_fps = requested_fps.load(std::memory_order_relaxed);
    if (severe_feedback_streak >= 3 &&
        now - last_fps_change >= std::chrono::seconds(3)) {
      const uint32_t lower = LowerFpsTier(current_fps, target_fps);
      if (lower < current_fps) {
        requested_fps.store(lower, std::memory_order_relaxed);
        last_fps_change = now;
      }
      severe_feedback_streak = 0;
    } else if (healthy_feedback_streak >= 8 &&
               now - last_fps_change >= std::chrono::seconds(8)) {
      const uint32_t higher = RaiseFpsTier(current_fps, target_fps);
      if (higher > current_fps) {
        requested_fps.store(higher, std::memory_order_relaxed);
        last_fps_change = now;
      }
      healthy_feedback_streak = 0;
    }
  };

  if (session_config.access_code.empty()) {
    std::wcerr << L"SECURITY: DESKLINK_ACCESS_CODE is not set; incoming remote-control offers will be rejected.\n";
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
             << L"Desktop: output " << output_index << L" ("
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
  bool was_connected = false;
  auto last_cached_recovery = clock::now() - std::chrono::seconds(1);

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

    // The track may still be opening. Preserve a pending keyframe request so
    // the first decodable frame is retried without waiting for desktop motion.
    keyframe_requested.store(true, std::memory_order_relaxed);
    return false;
  };

  while (g_running) {
    const bool connected = session.connected();
    if (connected && !was_connected && encoder_ready) {
      keyframe_requested.store(true, std::memory_order_relaxed);
    } else if (!connected && was_connected) {
      requested_fps.store(target_fps, std::memory_order_relaxed);
      if (encoder_ready && active_bitrate != target_bitrate) {
        if (encoder.SetBitrate(target_bitrate)) {
          active_bitrate = target_bitrate;
          requested_bitrate.store(target_bitrate, std::memory_order_relaxed);
        }
      }
    }
    was_connected = connected;

    const uint32_t desired_fps = connected
        ? requested_fps.load(std::memory_order_relaxed)
        : target_fps;
    if (desired_fps != active_fps) {
      active_fps = std::clamp<uint32_t>(desired_fps, 15, target_fps);
      if (!connected) requested_fps.store(target_fps, std::memory_order_relaxed);
      std::wcout << L"\nAdaptive frame-rate target: " << active_fps << L" fps\n";
    }

    if (connected && encoder_ready) {
      const uint32_t desired_bitrate = requested_bitrate.load(std::memory_order_relaxed);
      if (desired_bitrate != active_bitrate) {
        if (encoder.SetBitrate(desired_bitrate)) {
          active_bitrate = desired_bitrate;
        } else {
          requested_bitrate.store(active_bitrate, std::memory_order_relaxed);
        }
      }
    }

    bool encoded_fresh_frame = false;
    auto frame = capture.Acquire(16);

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
    }

    if (frame) {
      ++captured_frames;

      if (frame->width != source_width || frame->height != source_height) {
        source_width = frame->width;
        source_height = frame->height;
        encode_size = FitWithin(source_width, source_height, max_width, max_height);

        encoder.Reset();
        converter.Reset();
        converter_ready = converter.Initialize(
            device.Get(),
            source_width,
            source_height,
            encode_size.width,
            encode_size.height,
            target_fps);

        const uint32_t restart_bitrate = std::clamp(
            requested_bitrate.load(std::memory_order_relaxed),
            min_bitrate,
            target_bitrate);
        encoder_ready = converter_ready && encoder.Initialize(
            device.Get(),
            encode_size.width,
            encode_size.height,
            target_fps,
            restart_bitrate);
        active_bitrate = restart_bitrate;
        last_encoded_timestamp100ns = 0;
        last_fresh_encode_timestamp100ns = 0;
        last_cache_update_timestamp100ns = 0;

        if (encoder_ready) {
          keyframe_requested.store(true, std::memory_order_relaxed);
          std::wcout << L"\nDisplay changed; video pipeline rebuilt for "
                     << source_width << L"x" << source_height << L" -> "
                     << encode_size.width << L"x" << encode_size.height << L"\n";
        } else {
          std::wcerr << L"\nDisplay changed but video pipeline could not be rebuilt; input remains active.\n";
        }
      }

      const bool recovery_requested = keyframe_requested.load(std::memory_order_relaxed);
      const uint64_t min_frame_interval100ns =
          10'000'000ULL / std::max<uint32_t>(1, active_fps);
      const bool fresh_frame_due = last_fresh_encode_timestamp100ns == 0 ||
                                   frame->timestamp100ns >=
                                       last_fresh_encode_timestamp100ns + min_frame_interval100ns;

      // While idle, refresh the cached GPU frame at no more than 10 fps. While
      // connected, skip conversion work for frames that would be discarded by
      // the adaptive frame-rate target. Input still travels independently.
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

    // PLI/FIR, a new connection, or severe packet loss can request recovery at
    // a moment when DXGI has no new frame. Re-encode the cached GPU texture at
    // most four times per second until the RTP track accepts a decodable IDR.
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
