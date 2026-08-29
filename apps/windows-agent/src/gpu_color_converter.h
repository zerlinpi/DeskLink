#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace desklink {

class GpuColorConverter {
 public:
  bool Initialize(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t fps = 60);
  Microsoft::WRL::ComPtr<ID3D11Texture2D> Convert(ID3D11Texture2D* bgra_texture);
  void Reset();

 private:
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
  Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
  uint32_t width_{0};
  uint32_t height_{0};
};

}  // namespace desklink
