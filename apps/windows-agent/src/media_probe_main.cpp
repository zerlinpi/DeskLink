#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

#include "desktop_capture.h"
#include "gpu_color_converter.h"
#include "h264_encoder.h"

using Microsoft::WRL::ComPtr;

namespace {

struct VideoSize {
  uint32_t width{0};
  uint32_t height{0};
};

VideoSize FitWithin(
    uint32_t source_width,
    uint32_t source_height,
    uint32_t max_width,
    uint32_t max_height) {
  if (source_width == 0 || source_height == 0) return {};
  const double scale = std::min({
      1.0,
      static_cast<double>(max_width) / static_cast<double>(source_width),
      static_cast<double>(max_height) / static_cast<double>(source_height),
  });
  uint32_t width = std::max<uint32_t>(2, static_cast<uint32_t>(source_width * scale) & ~1U);
  uint32_t height = std::max<uint32_t>(2, static_cast<uint32_t>(source_height * scale) & ~1U);
  return {width, height};
}

uint64_t Now100ns() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() / 100);
}

std::wstring AdapterName(ID3D11Device* device) {
  if (!device) return L"<unknown>";
  ComPtr<IDXGIDevice> dxgi_device;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) return L"<unknown>";
  ComPtr<IDXGIAdapter> adapter;
  if (FAILED(dxgi_device->GetAdapter(&adapter)) || !adapter) return L"<unknown>";
  DXGI_ADAPTER_DESC description{};
  if (FAILED(adapter->GetDesc(&description))) return L"<unknown>";
  return description.Description;
}

ComPtr<ID3D11Texture2D> CreateSyntheticDesktopTexture(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    uint32_t width,
    uint32_t height) {
  if (!device || !context || width == 0 || height == 0) return {};

  D3D11_TEXTURE2D_DESC texture_desc{};
  texture_desc.Width = width;
  texture_desc.Height = height;
  texture_desc.MipLevels = 1;
  texture_desc.ArraySize = 1;
  texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  texture_desc.SampleDesc.Count = 1;
  texture_desc.Usage = D3D11_USAGE_DEFAULT;
  texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(device->CreateTexture2D(&texture_desc, nullptr, &texture)) || !texture) return {};

  ComPtr<ID3D11RenderTargetView> render_target;
  if (FAILED(device->CreateRenderTargetView(texture.Get(), nullptr, &render_target)) || !render_target) {
    return {};
  }
  const FLOAT background[4] = {0.08f, 0.16f, 0.30f, 1.0f};
  context->ClearRenderTargetView(render_target.Get(), background);
  context->Flush();
  return texture;
}

int Fail(int code, const std::wstring& message) {
  std::wcerr << L"[FAIL] " << message << L"\n";
  return code;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  uint32_t output_index = 0;
  if (argc > 1) {
    try {
      output_index = static_cast<uint32_t>(std::stoul(argv[1]));
    } catch (...) {
      std::wcerr << L"Usage: desklink-media-probe.exe [output-index]\n";
      return 2;
    }
  }

  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool should_uninitialize_com = SUCCEEDED(com);
  if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
    return Fail(10, L"COM initialization failed");
  }

  const HRESULT mf = MFStartup(MF_VERSION);
  if (FAILED(mf)) {
    if (should_uninitialize_com) CoUninitialize();
    return Fail(11, L"Media Foundation initialization failed");
  }

  auto cleanup = [&]() {
    MFShutdown();
    if (should_uninitialize_com) CoUninitialize();
  };

  UINT device_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
  const D3D_FEATURE_LEVEL requested_levels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };
  D3D_FEATURE_LEVEL selected_level{};
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  HRESULT hr = D3D11CreateDevice(
      nullptr,
      D3D_DRIVER_TYPE_HARDWARE,
      nullptr,
      device_flags,
      requested_levels,
      ARRAYSIZE(requested_levels),
      D3D11_SDK_VERSION,
      &device,
      &selected_level,
      &context);
  if (FAILED(hr) || !device || !context) {
    cleanup();
    return Fail(20, L"D3D11 hardware device creation failed. Update the GPU driver and avoid running through a headless/non-GPU session.");
  }

  std::wcout << L"[PASS] D3D11 hardware device: " << AdapterName(device.Get())
             << L" (feature level 0x" << std::hex << static_cast<unsigned int>(selected_level)
             << std::dec << L")\n";

  desklink::DesktopCapture capture;
  if (!capture.Initialize(device.Get(), output_index)) {
    cleanup();
    return Fail(
        30,
        L"DXGI Desktop Duplication initialization failed. Run DeskLink in an interactive Windows desktop session and update the display driver.");
  }

  std::wcout << L"[PASS] DXGI Desktop Duplication output " << output_index
             << L": " << capture.width() << L"x" << capture.height()
             << L" at (" << capture.left() << L"," << capture.top() << L")\n";

  const auto displays = capture.EnumerateOutputs();
  std::wcout << L"[INFO] Display outputs detected: " << displays.size() << L"\n";

  const uint32_t source_width = capture.width();
  const uint32_t source_height = capture.height();
  const VideoSize encode_size = FitWithin(source_width, source_height, 1280, 720);
  if (encode_size.width == 0 || encode_size.height == 0) {
    cleanup();
    return Fail(31, L"DXGI output reported an invalid desktop size");
  }

  desklink::GpuColorConverter converter;
  if (!converter.Initialize(
          device.Get(),
          source_width,
          source_height,
          encode_size.width,
          encode_size.height,
          30)) {
    cleanup();
    return Fail(
        40,
        L"D3D11 Video Processor cannot initialize BGRA-to-NV12 conversion. Update the graphics driver or use a GPU with D3D11 video processing support.");
  }

  auto captured = capture.Acquire(800);
  ComPtr<ID3D11Texture2D> source_texture;
  uint64_t timestamp100ns = Now100ns();
  if (captured && captured->texture) {
    source_texture = captured->texture;
    timestamp100ns = captured->timestamp100ns;
    std::wcout << L"[PASS] Captured a live desktop frame\n";
  } else {
    source_texture = CreateSyntheticDesktopTexture(
        device.Get(),
        context.Get(),
        source_width,
        source_height);
    if (!source_texture) {
      cleanup();
      return Fail(41, L"Desktop frame timed out and the synthetic GPU test surface could not be created");
    }
    std::wcout << L"[WARN] No desktop change arrived within 800 ms; Desktop Duplication initialized, so the converter test will use a synthetic GPU frame\n";
  }

  ComPtr<ID3D11Texture2D> nv12 = converter.Convert(source_texture.Get());
  if (!nv12) {
    cleanup();
    return Fail(42, L"GPU BGRA-to-NV12 conversion failed");
  }
  std::wcout << L"[PASS] D3D11 GPU BGRA->NV12 conversion: "
             << encode_size.width << L"x" << encode_size.height << L"\n";

  desklink::H264Encoder encoder;
  if (!encoder.Initialize(
          device.Get(),
          encode_size.width,
          encode_size.height,
          30,
          4'000'000)) {
    cleanup();
    return Fail(
        50,
        L"No compatible Media Foundation hardware H.264 encoder is available. DeskLink video requires a hardware H.264 MFT.");
  }
  std::wcout << L"[PASS] Hardware H.264 encoder: "
             << (encoder.encoder_name().empty() ? L"<unknown>" : encoder.encoder_name())
             << L"\n";

  desklink::EncodedH264Frame encoded;
  if (!encoder.Encode(nv12.Get(), timestamp100ns, &encoded) || encoded.bytes.empty()) {
    cleanup();
    return Fail(51, L"Hardware H.264 encoder initialized but did not produce an access unit");
  }
  std::wcout << L"[PASS] Encoded H.264 access unit: " << encoded.bytes.size()
             << L" bytes" << (encoded.keyframe ? L" (keyframe)" : L"") << L"\n";

  encoder.Reset();
  capture.Reset();
  cleanup();
  std::wcout << L"MEDIA_PROBE_OK\n";
  return 0;
}
