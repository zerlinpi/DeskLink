#pragma once

#include <d3d11.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace desklink {

struct EncodedH264Frame {
  std::vector<uint8_t> bytes;
  uint64_t timestamp100ns{0};
  bool keyframe{false};
};

class H264Encoder {
 public:
  bool Initialize(
      ID3D11Device* device,
      uint32_t width,
      uint32_t height,
      uint32_t fps,
      uint32_t bitrate_bps);

  bool Encode(
      ID3D11Texture2D* nv12_texture,
      uint64_t timestamp100ns,
      EncodedH264Frame* output);

  bool RequestKeyframe();
  bool SetBitrate(uint32_t bitrate_bps);
  void Reset();

  bool ready() const { return transform_ != nullptr; }
  const std::wstring& encoder_name() const { return encoder_name_; }

 private:
  bool ConfigureCodecApi(uint32_t bitrate_bps);
  bool WaitForEvent(MediaEventType wanted);
  bool SubmitInput(ID3D11Texture2D* texture, uint64_t timestamp100ns);
  bool ReadOutput(EncodedH264Frame* output);

  Microsoft::WRL::ComPtr<IMFTransform> transform_;
  Microsoft::WRL::ComPtr<IMFMediaEventGenerator> event_generator_;
  Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> device_manager_;
  Microsoft::WRL::ComPtr<ICodecAPI> codec_api_;
  DWORD input_stream_id_{0};
  DWORD output_stream_id_{0};
  uint32_t width_{0};
  uint32_t height_{0};
  uint32_t fps_{0};
  uint32_t bitrate_bps_{0};
  uint64_t frame_duration100ns_{0};
  std::wstring encoder_name_;
};

}  // namespace desklink
