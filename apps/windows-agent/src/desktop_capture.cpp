#include "desktop_capture.h"

#include <windows.h>

#include <chrono>
#include <iostream>

namespace desklink {
namespace {

uint64_t Now100ns() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() / 100);
}

std::string WideToUtf8(const wchar_t* text) {
  if (!text || !*text) return {};
  const int required = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      text,
      -1,
      nullptr,
      0,
      nullptr,
      nullptr);
  if (required <= 1) return {};

  std::string result(static_cast<size_t>(required - 1), '\0');
  if (WideCharToMultiByte(
          CP_UTF8,
          WC_ERR_INVALID_CHARS,
          text,
          -1,
          result.data(),
          required,
          nullptr,
          nullptr) <= 0) {
    return {};
  }
  return result;
}

}  // namespace

bool DesktopCapture::Initialize(ID3D11Device* device, uint32_t output_index) {
  Reset();
  if (!device) return false;

  device_ = device;
  output_index_ = output_index;

  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
  HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(&dxgi_device));
  if (FAILED(hr)) {
    std::wcerr << L"DesktopCapture: IDXGIDevice query failed: 0x" << std::hex << hr << L"\n";
    return false;
  }

  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  hr = dxgi_device->GetAdapter(&adapter);
  if (FAILED(hr)) {
    std::wcerr << L"DesktopCapture: GetAdapter failed: 0x" << std::hex << hr << L"\n";
    return false;
  }

  hr = adapter.As(&adapter_);
  if (FAILED(hr)) {
    std::wcerr << L"DesktopCapture: IDXGIAdapter1 query failed: 0x" << std::hex << hr << L"\n";
    return false;
  }

  return RecreateDuplication();
}

void DesktopCapture::ReleaseAcquiredFrame() {
  if (duplication_ && frame_acquired_) {
    duplication_->ReleaseFrame();
  }
  frame_acquired_ = false;
}

bool DesktopCapture::RecreateDuplication() {
  ReleaseAcquiredFrame();
  duplication_.Reset();
  output_.Reset();

  if (!adapter_ || !device_) return false;

  Microsoft::WRL::ComPtr<IDXGIOutput> output;
  HRESULT hr = adapter_->EnumOutputs(output_index_, &output);
  if (FAILED(hr)) {
    std::wcerr << L"DesktopCapture: EnumOutputs(" << output_index_ << L") failed: 0x"
               << std::hex << hr << L"\n";
    return false;
  }

  hr = output.As(&output_);
  if (FAILED(hr)) {
    std::wcerr << L"DesktopCapture: IDXGIOutput1 query failed: 0x" << std::hex << hr << L"\n";
    return false;
  }

  DXGI_OUTPUT_DESC desc{};
  hr = output_->GetDesc(&desc);
  if (FAILED(hr)) return false;

  left_ = desc.DesktopCoordinates.left;
  top_ = desc.DesktopCoordinates.top;
  width_ = static_cast<uint32_t>(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left);
  height_ = static_cast<uint32_t>(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);

  hr = output_->DuplicateOutput(device_.Get(), &duplication_);
  if (FAILED(hr)) {
    std::wcerr << L"DesktopCapture: DuplicateOutput failed: 0x" << std::hex << hr
               << L". Ensure the process runs in an interactive desktop session.\n";
    return false;
  }

  return true;
}

std::vector<DisplayInfo> DesktopCapture::EnumerateOutputs() const {
  std::vector<DisplayInfo> displays;
  if (!adapter_) return displays;

  const POINT origin{0, 0};
  const HMONITOR primary_monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTONULL);

  for (uint32_t index = 0;; ++index) {
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    const HRESULT hr = adapter_->EnumOutputs(index, &output);
    if (hr == DXGI_ERROR_NOT_FOUND) break;
    if (FAILED(hr) || !output) continue;

    DXGI_OUTPUT_DESC desc{};
    if (FAILED(output->GetDesc(&desc))) continue;

    DisplayInfo info;
    info.index = index;
    info.name = WideToUtf8(desc.DeviceName);
    info.left = desc.DesktopCoordinates.left;
    info.top = desc.DesktopCoordinates.top;
    info.width = static_cast<uint32_t>(
        desc.DesktopCoordinates.right - desc.DesktopCoordinates.left);
    info.height = static_cast<uint32_t>(
        desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
    info.primary = primary_monitor != nullptr && desc.Monitor == primary_monitor;
    displays.push_back(std::move(info));
  }

  return displays;
}

bool DesktopCapture::SwitchOutput(uint32_t output_index) {
  if (!adapter_ || !device_) return false;
  if (output_index == output_index_ && duplication_) return true;

  const uint32_t previous_index = output_index_;
  output_index_ = output_index;
  if (RecreateDuplication()) return true;

  std::wcerr << L"DesktopCapture: unable to switch to output " << output_index
             << L"; restoring output " << previous_index << L"\n";
  output_index_ = previous_index;
  if (!RecreateDuplication()) {
    std::wcerr << L"DesktopCapture: failed to restore previous output after switch failure\n";
  }
  return false;
}

std::optional<CapturedFrame> DesktopCapture::Acquire(uint32_t timeout_ms) {
  if (!duplication_) return std::nullopt;

  ReleaseAcquiredFrame();

  DXGI_OUTDUPL_FRAME_INFO info{};
  Microsoft::WRL::ComPtr<IDXGIResource> resource;
  HRESULT hr = duplication_->AcquireNextFrame(timeout_ms, &info, &resource);

  if (hr == DXGI_ERROR_WAIT_TIMEOUT) return std::nullopt;
  if (hr == DXGI_ERROR_ACCESS_LOST) {
    RecreateDuplication();
    return std::nullopt;
  }
  if (FAILED(hr)) {
    std::wcerr << L"DesktopCapture: AcquireNextFrame failed: 0x" << std::hex << hr << L"\n";
    return std::nullopt;
  }

  frame_acquired_ = true;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  hr = resource.As(&texture);
  if (FAILED(hr)) {
    ReleaseAcquiredFrame();
    return std::nullopt;
  }

  D3D11_TEXTURE2D_DESC tex_desc{};
  texture->GetDesc(&tex_desc);

  CapturedFrame frame;
  frame.texture = std::move(texture);
  frame.width = tex_desc.Width;
  frame.height = tex_desc.Height;
  frame.timestamp100ns = Now100ns();
  return frame;
}

void DesktopCapture::Reset() {
  ReleaseAcquiredFrame();
  duplication_.Reset();
  output_.Reset();
  adapter_.Reset();
  device_.Reset();
  output_index_ = 0;
  width_ = 0;
  height_ = 0;
  left_ = 0;
  top_ = 0;
}

}  // namespace desklink
