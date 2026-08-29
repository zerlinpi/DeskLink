#include "desktop_capture.h"

#include <chrono>
#include <iostream>

namespace desklink {
namespace {

uint64_t Now100ns() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() / 100);
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

bool DesktopCapture::RecreateDuplication() {
  duplication_.Reset();
  output_.Reset();
  frame_acquired_ = false;

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

std::optional<CapturedFrame> DesktopCapture::Acquire(uint32_t timeout_ms) {
  if (!duplication_) return std::nullopt;

  if (frame_acquired_) {
    duplication_->ReleaseFrame();
    frame_acquired_ = false;
  }

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
    duplication_->ReleaseFrame();
    frame_acquired_ = false;
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
  if (duplication_ && frame_acquired_) {
    duplication_->ReleaseFrame();
  }
  frame_acquired_ = false;
  duplication_.Reset();
  output_.Reset();
  adapter_.Reset();
  device_.Reset();
  width_ = 0;
  height_ = 0;
}

}  // namespace desklink
