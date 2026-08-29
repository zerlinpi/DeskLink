#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

#include <rtc/rtc.hpp>

#include "desktop_capture.h"
#include "webrtc_session.h"

using Microsoft::WRL::ComPtr;

namespace {

std::atomic_bool g_running{true};

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

uint16_t EnvPortOr(const char* name, uint16_t fallback) {
  const std::string value = EnvOr(name, "");
  if (value.empty()) return fallback;
  try {
    const unsigned long parsed = std::stoul(value);
    if (parsed <= 65535) return static_cast<uint16_t>(parsed);
  } catch (...) {
  }
  return fallback;
}

std::string DefaultDeviceId() {
  char computer_name[MAX_COMPUTERNAME_LENGTH + 1]{};
  DWORD size = static_cast<DWORD>(std::size(computer_name));
  if (GetComputerNameA(computer_name, &size) && size > 0) {
    return std::string("win-") + computer_name;
  }
  return "windows-host";
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

  desklink::SessionConfig session_config;
  session_config.signal_url = EnvOr("DESKLINK_SIGNAL_URL", session_config.signal_url);
  session_config.device_id = EnvOr("DESKLINK_DEVICE_ID", DefaultDeviceId());
  session_config.stun_url = EnvOr("DESKLINK_STUN_URL", session_config.stun_url);
  session_config.turn_host = EnvOr("DESKLINK_TURN_HOST", session_config.turn_host);
  session_config.turn_port = EnvPortOr("DESKLINK_TURN_PORT", session_config.turn_port);
  session_config.turn_username = EnvOr("DESKLINK_TURN_USERNAME", session_config.turn_username);
  session_config.turn_password = EnvOr("DESKLINK_TURN_PASSWORD", session_config.turn_password);

  rtc::InitLogger(rtc::LogLevel::Info);
  desklink::WebRtcSession session(std::move(session_config));
  session.Start();

  std::wcout << L"DeskLink Windows Agent\n"
             << L"GPU: " << adapter_desc.Description << L"\n"
             << L"Output: " << output_index << L" (" << capture.width() << L"x" << capture.height() << L")\n"
             << L"DXGI Desktop Duplication is active. Press Ctrl+C to stop.\n";

  using clock = std::chrono::steady_clock;
  auto window_start = clock::now();
  uint64_t captured_frames = 0;
  uint64_t timeout_ticks = 0;

  while (g_running) {
    auto frame = capture.Acquire(16);
    if (frame) {
      ++captured_frames;
      // M1 next step: GPU color conversion -> Media Foundation H.264 -> WebRTC video track.
    } else {
      ++timeout_ticks;
    }

    const auto now = clock::now();
    const auto elapsed = std::chrono::duration<double>(now - window_start).count();
    if (elapsed >= 1.0) {
      const double fps = static_cast<double>(captured_frames) / elapsed;
      std::wcout << L"capture=" << std::fixed << std::setprecision(1) << fps
                 << L" fps, idle/timeouts=" << timeout_ticks
                 << (session.connected() ? L", controller=connected" : L", controller=waiting")
                 << L"\r" << std::flush;
      captured_frames = 0;
      timeout_ticks = 0;
      window_start = now;
    }
  }

  std::wcout << L"\nDeskLink Agent stopped.\n";
  session.Stop();
  capture.Reset();
  context->ClearState();
  context->Flush();
  MFShutdown();
  CoUninitialize();
  return 0;
}
