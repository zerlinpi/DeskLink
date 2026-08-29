#include "h264_encoder.h"

#include <mfapi.h>
#include <mferror.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>

namespace desklink {
namespace {

bool SetCodecUInt32(ICodecAPI* codec, const GUID& key, uint32_t value) {
  if (!codec) return false;
  VARIANT variant;
  VariantInit(&variant);
  variant.vt = VT_UI4;
  variant.ulVal = value;
  return SUCCEEDED(codec->SetValue(&key, &variant));
}

bool SetCodecBool(ICodecAPI* codec, const GUID& key, bool value) {
  if (!codec) return false;
  VARIANT variant;
  VariantInit(&variant);
  variant.vt = VT_BOOL;
  variant.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
  return SUCCEEDED(codec->SetValue(&key, &variant));
}

bool StartsWithParameterSet(const std::vector<uint8_t>& bytes) {
  for (size_t i = 0; i + 4 < bytes.size() && i < 64; ++i) {
    size_t nalu = std::numeric_limits<size_t>::max();
    if (i + 4 < bytes.size() && bytes[i] == 0 && bytes[i + 1] == 0 &&
        bytes[i + 2] == 0 && bytes[i + 3] == 1) {
      nalu = i + 4;
    } else if (i + 3 < bytes.size() && bytes[i] == 0 && bytes[i + 1] == 0 &&
               bytes[i + 2] == 1) {
      nalu = i + 3;
    }
    if (nalu != std::numeric_limits<size_t>::max() && nalu < bytes.size()) {
      const uint8_t type = bytes[nalu] & 0x1f;
      return type == 7 || type == 8;
    }
  }
  return false;
}

}  // namespace

bool H264Encoder::Initialize(
    ID3D11Device* device,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    uint32_t bitrate_bps) {
  Reset();
  if (!device || width == 0 || height == 0 || fps == 0 || bitrate_bps == 0) return false;

  UINT reset_token = 0;
  HRESULT hr = MFCreateDXGIDeviceManager(&reset_token, &device_manager_);
  if (FAILED(hr)) {
    std::wcerr << L"H264Encoder: MFCreateDXGIDeviceManager failed: 0x" << std::hex << hr << L"\n";
    Reset();
    return false;
  }
  hr = device_manager_->ResetDevice(device, reset_token);
  if (FAILED(hr)) {
    std::wcerr << L"H264Encoder: ResetDevice failed: 0x" << std::hex << hr << L"\n";
    Reset();
    return false;
  }

  MFT_REGISTER_TYPE_INFO output_info{MFMediaType_Video, MFVideoFormat_H264};
  IMFActivate** activations = nullptr;
  UINT32 activation_count = 0;
  hr = MFTEnumEx(
      MFT_CATEGORY_VIDEO_ENCODER,
      MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
      nullptr,
      &output_info,
      &activations,
      &activation_count);
  if (FAILED(hr) || activation_count == 0 || !activations) {
    std::wcerr << L"H264Encoder: no hardware H264 Media Foundation encoder found\n";
    if (activations) CoTaskMemFree(activations);
    Reset();
    return false;
  }

  Microsoft::WRL::ComPtr<IMFActivate> activation;
  activation.Attach(activations[0]);
  for (UINT32 i = 1; i < activation_count; ++i) {
    activations[i]->Release();
  }
  CoTaskMemFree(activations);

  WCHAR* allocated_name = nullptr;
  UINT32 name_length = 0;
  if (SUCCEEDED(activation->GetAllocatedString(
          MFT_FRIENDLY_NAME_Attribute,
          &allocated_name,
          &name_length)) &&
      allocated_name) {
    encoder_name_.assign(allocated_name, name_length);
    CoTaskMemFree(allocated_name);
  }

  hr = activation->ActivateObject(IID_PPV_ARGS(&transform_));
  if (FAILED(hr)) {
    std::wcerr << L"H264Encoder: activation failed: 0x" << std::hex << hr << L"\n";
    Reset();
    return false;
  }

  Microsoft::WRL::ComPtr<IMFAttributes> attributes;
  hr = transform_->GetAttributes(&attributes);
  if (FAILED(hr)) {
    std::wcerr << L"H264Encoder: GetAttributes failed: 0x" << std::hex << hr << L"\n";
    Reset();
    return false;
  }

  // Hardware encoder MFTs are normally asynchronous and must be explicitly unlocked.
  UINT32 is_async = FALSE;
  if (SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)) && is_async) {
    hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    if (FAILED(hr)) {
      std::wcerr << L"H264Encoder: async unlock failed: 0x" << std::hex << hr << L"\n";
      Reset();
      return false;
    }
  }
  attributes->SetUINT32(MF_LOW_LATENCY, TRUE);

  hr = transform_.As(&event_generator_);
  if (FAILED(hr) || !event_generator_) {
    std::wcerr << L"H264Encoder: selected hardware encoder has no async event generator\n";
    Reset();
    return false;
  }

  hr = transform_->GetStreamIDs(1, &input_stream_id_, 1, &output_stream_id_);
  if (hr == E_NOTIMPL) {
    input_stream_id_ = 0;
    output_stream_id_ = 0;
    hr = S_OK;
  }
  if (FAILED(hr)) {
    std::wcerr << L"H264Encoder: GetStreamIDs failed: 0x" << std::hex << hr << L"\n";
    Reset();
    return false;
  }

  hr = transform_->ProcessMessage(
      MFT_MESSAGE_SET_D3D_MANAGER,
      reinterpret_cast<ULONG_PTR>(device_manager_.Get()));
  if (FAILED(hr) && hr != E_NOTIMPL) {
    std::wcerr << L"H264Encoder: D3D manager rejected: 0x" << std::hex << hr << L"\n";
    Reset();
    return false;
  }

  transform_.As(&codec_api_);
  width_ = width;
  height_ = height;
  fps_ = fps;
  bitrate_bps_ = bitrate_bps;
  frame_duration100ns_ = 10'000'000ULL / fps;
  ConfigureCodecApi(bitrate_bps);

  Microsoft::WRL::ComPtr<IMFMediaType> output_type;
  hr = MFCreateMediaType(&output_type);
  if (FAILED(hr) ||
      FAILED(output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
      FAILED(output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)) ||
      FAILED(output_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate_bps)) ||
      FAILED(MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, width, height)) ||
      FAILED(MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, fps, 1)) ||
      FAILED(output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
      FAILED(output_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base)) ||
      FAILED(MFSetAttributeRatio(output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1))) {
    std::wcerr << L"H264Encoder: failed to configure H264 output media type\n";
    Reset();
    return false;
  }

  hr = transform_->SetOutputType(output_stream_id_, output_type.Get(), 0);
  if (FAILED(hr)) {
    std::wcerr << L"H264Encoder: SetOutputType failed: 0x" << std::hex << hr << L"\n";
    Reset();
    return false;
  }
  CacheSequenceHeader();

  Microsoft::WRL::ComPtr<IMFMediaType> input_type;
  hr = MFCreateMediaType(&input_type);
  if (FAILED(hr) ||
      FAILED(input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
      FAILED(input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12)) ||
      FAILED(MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, width, height)) ||
      FAILED(MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, fps, 1)) ||
      FAILED(input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
      FAILED(MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1))) {
    std::wcerr << L"H264Encoder: failed to configure NV12 input media type\n";
    Reset();
    return false;
  }

  hr = transform_->SetInputType(input_stream_id_, input_type.Get(), 0);
  if (FAILED(hr)) {
    std::wcerr << L"H264Encoder: SetInputType(NV12) failed: 0x" << std::hex << hr << L"\n";
    Reset();
    return false;
  }

  hr = transform_->GetOutputStreamInfo(output_stream_id_, &output_stream_info_);
  if (FAILED(hr)) {
    std::wcerr << L"H264Encoder: GetOutputStreamInfo failed: 0x" << std::hex << hr << L"\n";
    Reset();
    return false;
  }

  transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  if (FAILED(hr)) {
    std::wcerr << L"H264Encoder: BEGIN_STREAMING failed: 0x" << std::hex << hr << L"\n";
    Reset();
    return false;
  }
  hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  if (FAILED(hr)) {
    std::wcerr << L"H264Encoder: START_OF_STREAM failed: 0x" << std::hex << hr << L"\n";
    Reset();
    return false;
  }

  std::wcout << L"H264 hardware encoder: "
             << (encoder_name_.empty() ? L"<unknown>" : encoder_name_)
             << L", " << width << L"x" << height << L" @ " << fps
             << L" fps, " << (bitrate_bps / 1'000'000.0) << L" Mbps\n";
  return true;
}

bool H264Encoder::ConfigureCodecApi(uint32_t bitrate_bps) {
  if (!codec_api_) return false;

  // Unsupported knobs are intentionally non-fatal because vendors expose slightly
  // different ICodecAPI capabilities. The two critical settings are attempted first.
  SetCodecBool(codec_api_.Get(), CODECAPI_AVLowLatencyMode, true);
  SetCodecUInt32(
      codec_api_.Get(),
      CODECAPI_AVEncCommonRateControlMode,
      static_cast<uint32_t>(eAVEncCommonRateControlMode_CBR));
  SetCodecUInt32(codec_api_.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate_bps);
  SetCodecUInt32(codec_api_.Get(), CODECAPI_AVEncMPVGOPSize, fps_ == 0 ? 60 : fps_);
  SetCodecUInt32(codec_api_.Get(), CODECAPI_AVEncCommonQualityVsSpeed, 25);
  SetCodecUInt32(codec_api_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
  return true;
}

bool H264Encoder::CacheSequenceHeader() {
  sequence_header_.clear();
  if (!transform_) return false;

  Microsoft::WRL::ComPtr<IMFMediaType> current;
  if (FAILED(transform_->GetOutputCurrentType(output_stream_id_, &current))) return false;

  UINT32 size = 0;
  if (FAILED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) || size == 0) return false;

  sequence_header_.resize(size);
  UINT32 written = 0;
  if (FAILED(current->GetBlob(
          MF_MT_MPEG_SEQUENCE_HEADER,
          sequence_header_.data(),
          size,
          &written))) {
    sequence_header_.clear();
    return false;
  }
  sequence_header_.resize(written);
  return !sequence_header_.empty();
}

bool H264Encoder::WaitForEvent(MediaEventType wanted) {
  if (!event_generator_) return false;

  if (wanted == METransformNeedInput && need_input_pending_) {
    need_input_pending_ = false;
    return true;
  }
  if (wanted == METransformHaveOutput && have_output_pending_) {
    have_output_pending_ = false;
    return true;
  }

  for (;;) {
    Microsoft::WRL::ComPtr<IMFMediaEvent> event;
    HRESULT hr = event_generator_->GetEvent(0, &event);
    if (FAILED(hr)) {
      std::wcerr << L"H264Encoder: GetEvent failed: 0x" << std::hex << hr << L"\n";
      return false;
    }

    HRESULT event_status = S_OK;
    if (SUCCEEDED(event->GetStatus(&event_status)) && FAILED(event_status)) {
      std::wcerr << L"H264Encoder: encoder event failed: 0x" << std::hex << event_status << L"\n";
      return false;
    }

    MediaEventType type = MEUnknown;
    if (FAILED(event->GetType(&type))) return false;
    if (type == wanted) return true;

    if (type == METransformNeedInput) {
      need_input_pending_ = true;
    } else if (type == METransformHaveOutput) {
      have_output_pending_ = true;
    } else if (type == MEError) {
      return false;
    }
  }
}

bool H264Encoder::SubmitInput(ID3D11Texture2D* texture, uint64_t timestamp100ns) {
  if (!texture || !transform_) return false;

  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  HRESULT hr = MFCreateDXGISurfaceBuffer(
      IID_ID3D11Texture2D,
      texture,
      0,
      FALSE,
      &buffer);
  if (FAILED(hr)) {
    std::wcerr << L"H264Encoder: MFCreateDXGISurfaceBuffer failed: 0x" << std::hex << hr << L"\n";
    return false;
  }

  Microsoft::WRL::ComPtr<IMFSample> sample;
  hr = MFCreateSample(&sample);
  if (FAILED(hr) || FAILED(sample->AddBuffer(buffer.Get()))) return false;

  sample->SetSampleTime(static_cast<LONGLONG>(timestamp100ns));
  sample->SetSampleDuration(static_cast<LONGLONG>(frame_duration100ns_));

  hr = transform_->ProcessInput(input_stream_id_, sample.Get(), 0);
  if (FAILED(hr)) {
    std::wcerr << L"H264Encoder: ProcessInput failed: 0x" << std::hex << hr << L"\n";
    return false;
  }
  return true;
}

bool H264Encoder::ReadOutput(EncodedH264Frame* output) {
  if (!output || !transform_) return false;
  output->bytes.clear();
  output->timestamp100ns = 0;
  output->keyframe = false;

  Microsoft::WRL::ComPtr<IMFSample> provided_sample;
  if ((output_stream_info_.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
    HRESULT hr = MFCreateSample(&provided_sample);
    if (FAILED(hr)) return false;

    const DWORD buffer_size = std::max<DWORD>(
        output_stream_info_.cbSize,
        std::max<DWORD>(1 << 20, bitrate_bps_ / std::max<uint32_t>(1, fps_) * 4));
    Microsoft::WRL::ComPtr<IMFMediaBuffer> provided_buffer;
    hr = MFCreateMemoryBuffer(buffer_size, &provided_buffer);
    if (FAILED(hr) || FAILED(provided_sample->AddBuffer(provided_buffer.Get()))) return false;
  }

  MFT_OUTPUT_DATA_BUFFER data{};
  data.dwStreamID = output_stream_id_;
  data.pSample = provided_sample.Get();
  DWORD status = 0;
  HRESULT hr = transform_->ProcessOutput(0, 1, &data, &status);

  Microsoft::WRL::ComPtr<IMFCollection> events;
  if (data.pEvents) {
    events.Attach(data.pEvents);
  }

  if (FAILED(hr)) {
    if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT) {
      std::wcerr << L"H264Encoder: ProcessOutput failed: 0x" << std::hex << hr << L"\n";
    }
    if (data.pSample && data.pSample != provided_sample.Get()) data.pSample->Release();
    return false;
  }

  Microsoft::WRL::ComPtr<IMFSample> sample;
  if (provided_sample && data.pSample == provided_sample.Get()) {
    sample = provided_sample;
  } else if (data.pSample) {
    sample.Attach(data.pSample);
  }
  if (!sample) return false;

  LONGLONG pts = 0;
  if (SUCCEEDED(sample->GetSampleTime(&pts)) && pts >= 0) {
    output->timestamp100ns = static_cast<uint64_t>(pts);
  }

  UINT32 clean_point = FALSE;
  if (SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean_point))) {
    output->keyframe = clean_point != FALSE;
  }

  Microsoft::WRL::ComPtr<IMFMediaBuffer> contiguous;
  hr = sample->ConvertToContiguousBuffer(&contiguous);
  if (FAILED(hr)) return false;

  BYTE* bytes = nullptr;
  DWORD max_length = 0;
  DWORD current_length = 0;
  hr = contiguous->Lock(&bytes, &max_length, &current_length);
  if (FAILED(hr) || !bytes || current_length == 0) {
    if (SUCCEEDED(hr)) contiguous->Unlock();
    return false;
  }

  output->bytes.assign(bytes, bytes + current_length);
  contiguous->Unlock();

  if (output->keyframe && !sequence_header_.empty() && !StartsWithParameterSet(output->bytes)) {
    std::vector<uint8_t> with_header;
    with_header.reserve(sequence_header_.size() + output->bytes.size());
    with_header.insert(with_header.end(), sequence_header_.begin(), sequence_header_.end());
    with_header.insert(with_header.end(), output->bytes.begin(), output->bytes.end());
    output->bytes.swap(with_header);
  }

  return !output->bytes.empty();
}

bool H264Encoder::Encode(
    ID3D11Texture2D* nv12_texture,
    uint64_t timestamp100ns,
    EncodedH264Frame* output) {
  if (!ready() || !output || !nv12_texture) return false;
  if (!WaitForEvent(METransformNeedInput)) return false;
  if (!SubmitInput(nv12_texture, timestamp100ns)) return false;
  if (!WaitForEvent(METransformHaveOutput)) return false;
  return ReadOutput(output);
}

bool H264Encoder::RequestKeyframe() {
  return SetCodecBool(codec_api_.Get(), CODECAPI_AVEncVideoForceKeyFrame, true);
}

bool H264Encoder::SetBitrate(uint32_t bitrate_bps) {
  if (bitrate_bps == 0 || !codec_api_) return false;
  if (!SetCodecUInt32(codec_api_.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate_bps)) {
    return false;
  }
  bitrate_bps_ = bitrate_bps;
  return true;
}

void H264Encoder::Reset() {
  if (transform_) {
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  }

  codec_api_.Reset();
  event_generator_.Reset();
  transform_.Reset();
  device_manager_.Reset();
  output_stream_info_ = {};
  need_input_pending_ = false;
  have_output_pending_ = false;
  input_stream_id_ = 0;
  output_stream_id_ = 0;
  width_ = 0;
  height_ = 0;
  fps_ = 0;
  bitrate_bps_ = 0;
  frame_duration100ns_ = 0;
  sequence_header_.clear();
  encoder_name_.clear();
}

}  // namespace desklink
