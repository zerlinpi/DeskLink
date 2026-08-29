#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace desklink {

class GpuColorConverter {
 public:
  bool Initialize(
      ID3D11Device* device,
      uint32_t input_width,
      uint32_t input_height,
      uint32_t output_width,
      uint32_t output_height,
      uint32_t fps = 60);
  Microsoft::WRL::ComPtr<ID3D11Texture2D> Convert(ID3D11Texture2D* bgra_texture);
  Microsoft::WRL::ComPtr<ID3D11Texture2D> LatestFrame() const;
  void Reset();

 private:
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
  Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12_texture_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view_;
  uint32_t input_width_{0};
  uint32_t input_height_{0};
  uint32_t output_width_{0};
  uint32_t output_height_{0};
  bool has_frame_{false};
};

}  // namespace desklink
