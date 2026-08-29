#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <optional>

namespace desklink {

struct CapturedFrame {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  uint32_t width{0};
  uint32_t height{0};
  uint64_t timestamp100ns{0};
};

class DesktopCapture {
 public:
  DesktopCapture() = default;
  DesktopCapture(const DesktopCapture&) = delete;
  DesktopCapture& operator=(const DesktopCapture&) = delete;

  bool Initialize(ID3D11Device* device, uint32_t output_index = 0);
  std::optional<CapturedFrame> Acquire(uint32_t timeout_ms);
  void Reset();

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }

 private:
  bool RecreateDuplication();

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
  Microsoft::WRL::ComPtr<IDXGIOutput1> output_;
  Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
  uint32_t output_index_{0};
  uint32_t width_{0};
  uint32_t height_{0};
  bool frame_acquired_{false};
};

}  // namespace desklink
