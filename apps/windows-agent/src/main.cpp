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

std::string EnvOr(const char* name, std::string fallback) {
  if (const char* value = std::getenv(name); value && *value) return value;
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
  const uint32_t max_width = EnvUIntOr("DESKLINK_MAX_WIDTH", 1920, 640, 3840);
  const uint32_t max_height = EnvUIntOr("DESKLINK_MAX_HEIGHT", 1080, 360, 2160);
  const VideoSize encode_size = FitWithin(
      capture.width(),
      capture.height(),
      max_width,
      max_height);

  desklink::GpuColorConverter converter;
  const bool converter_ready = converter.Initialize(
      device.Get(),
      capture.width(),
      capture.height(),
      encode_size.width,
      encode_size.height,
      target_fps);

  desklink::H264Encoder encoder;
  const bool encoder_ready = converter_ready && encoder.Initialize(
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

  desklink::SessionConfig session_config;
  session_config.signal_url = EnvOr("DESKLINK_SIGNAL_URL", session_config.signal_url);
  session_config.device_id = EnvOr("DESKLINK_DEVICE_ID", DefaultDeviceId());
  session_config.access_code = EnvOr("DESKLINK_ACCESS_CODE", "");
  session_config.stun_url = EnvOr("DESKLINK_STUN_URL", session_config.stun_url);
  session_config.turn_host = EnvOr("DESKLINK_TURN_HOST", session_config.turn_host);
  session_config.turn_port = EnvPortOr("DESKLINK_TURN_PORT", session_config.turn_port);
  session_config.turn_username = EnvOr("DESKLINK_TURN_USERNAME", session_config.turn_username);
  session_config.turn_password = EnvOr("DESKLINK_TURN_PASSWORD", session_config.turn_password);

  if (session_config.access_code.empty()) {
    std::wcerr << L"SECURITY: DESKLINK_ACCESS_CODE is not set; incoming remote-control offers will be rejected.\n";
  }

  rtc::InitLogger(rtc::LogLevel::Info);
  desklink::WebRtcSession session(std::move(session_config));
  session.Start();

  std::wcout << L"DeskLink Windows Agent\n"
             << L"GPU: " << adapter_desc.Description << L"\n"
             << L"Desktop: output " << output_index << L" ("
             << capture.width() << L"x" << capture.height() << L")\n"
             << L"Stream target: " << encode_size.width << L"x" << encode_size.height
             << L" @ " << target_fps << L" fps, "
             << std::fixed << std::setprecision(1)
             << (target_bitrate / 1'000'000.0) << L" Mbps\n"
             << L"Press Ctrl+C to stop.\n";

  using clock = std::chrono::steady_clock;
  auto window_start = clock::now();
  uint64_t captured_frames = 0;
  uint64_t encoded_frames = 0;
  uint64_t sent_frames = 0;
  uint64_t encoded_bytes = 0;
  uint64_t timeout_ticks = 0;
  bool was_connected = false;

  while (g_running) {
    const bool connected = session.connected();
    if (connected && !was_connected && encoder_ready) {
      encoder.RequestKeyframe();
    }
    was_connected = connected;

    auto frame = capture.Acquire(16);
    if (frame) {
      ++captured_frames;

      // Do not consume encoder/GPU time while no controller is connected.
      if (connected && encoder_ready) {
        auto nv12 = converter.Convert(frame->texture.Get());
        if (nv12) {
          desklink::EncodedH264Frame encoded;
          if (encoder.Encode(nv12.Get(), frame->timestamp100ns, &encoded)) {
            ++encoded_frames;
            encoded_bytes += encoded.bytes.size();
            if (session.SendH264AccessUnit(
                    encoded.bytes.data(),
                    encoded.bytes.size(),
                    encoded.timestamp100ns)) {
              ++sent_frames;
            } else {
              // If the RTP track is not open yet, make the next successfully
              // delivered frame an IDR so the browser never starts from a P-frame.
              encoder.RequestKeyframe();
            }
          }
        }
      }
    } else {
      ++timeout_ticks;
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
