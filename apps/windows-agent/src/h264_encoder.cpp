#include "h264_encoder.h"

#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#include "h264_annexb.h"

namespace desklink {
namespace {

constexpr auto kEncoderEventTimeout = std::chrono::milliseconds(750);
constexpr auto kEncoderEventPollInterval = std::chrono::milliseconds(1);

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

void ReleaseActivations(IMFActivate** activations, UINT32 activation_count) {
  if (!activations) return;
  for (UINT32 i = 0; i < activation_count; ++i) {
    if (activations[i]) activations[i]->Release();
  }
  CoTaskMemFree(activations);
}

bool TryGetAdapterLuid(ID3D11Device* device, LUID* adapter_luid) {
  if (!device || !adapter_luid) return false;

  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) || !dxgi_device) return false;

  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  if (FAILED(dxgi_device->GetAdapter(&adapter)) || !adapter) return false;

  DXGI_ADAPTER_DESC description{};
  if (FAILED(adapter->GetDesc(&description))) return false;
  *adapter_luid = description.AdapterLuid;
  return true;
}

HRESULT EnumerateAdapterHardwareH264Encoders(
    const LUID& adapter_luid,
    IMFActivate*** activations,
    UINT32* activation_count) {
  if (!activations || !activation_count) return E_POINTER;
  *activations = nullptr;
  *activation_count = 0;

  Microsoft::WRL::ComPtr<IMFAttributes> attributes;
  HRESULT hr = MFCreateAttributes(&attributes, 1);
  if (FAILED(hr)) return hr;

  hr = attributes->SetBlob(
      MFT_ENUM_ADAPTER_LUID,
      reinterpret_cast<const UINT8*>(&adapter_luid),
      sizeof(adapter_luid));
  if (FAILED(hr)) return hr;

  MFT_REGISTER_TYPE_INFO output_info{MFMediaType_Video, MFVideoFormat_H264};
  return MFTEnum2(
      MFT_CATEGORY_VIDEO_ENCODER,
      MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
      nullptr,
      &output_info,
      attributes.Get(),
      activations,
      activation_count);

}

struct ActivatedEncoderCandidate {
  Microsoft::WRL::ComPtr<IMFTransform> transform;
  Microsoft::WRL::ComPtr<IMFMediaEventGenerator> event_generator;
  Microsoft::WRL::ComPtr<ICodecAPI> codec_api;
  DWORD input_stream_id{0};
  DWORD output_stream_id{0};
  MFT_OUTPUT_STREAM_INFO output_stream_info{};
  std::wstring name;
  bool adapter_matched{false};
};

std::wstring ReadActivationName(IMFActivate* activation) {
  if (!activation) return {};

  WCHAR* allocated_name = nullptr;
  UINT32 name_length = 0;
  std::wstring name;
  if (SUCCEEDED(activation->GetAllocatedString(
          MFT_FRIENDLY_NAME_Attribute,
          &allocated_name,
          &name_length)) &&
      allocated_name) {
    name.assign(allocated_name, name_length);
    CoTaskMemFree(allocated_name);
  }
  return name;
}

bool ConfigureCandidateCodecApi(ICodecAPI* codec, uint32_t fps, uint32_t bitrate_bps) {
  if (!codec) return false;

  // Vendor-specific support varies, so these tuning knobs intentionally remain
  // best-effort. Media type and stream-start compatibility below are mandatory.
  SetCodecBool(codec, CODECAPI_AVLowLatencyMode, true);
  SetCodecUInt32(
      codec,
      CODECAPI_AVEncCommonRateControlMode,
      static_cast<uint32_t>(eAVEncCommonRateControlMode_CBR));
  SetCodecUInt32(codec, CODECAPI_AVEncCommonMeanBitRate, bitrate_bps);
  SetCodecUInt32(codec, CODECAPI_AVEncMPVGOPSize, fps == 0 ? 60 : fps);
  SetCodecUInt32(codec, CODECAPI_AVEncCommonQualityVsSpeed, 25);
  SetCodecUInt32(codec, CODECAPI_AVEncMPVDefaultBPictureCount, 0);
  return true;
}

HRESULT BuildH264OutputMediaType(
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    uint32_t bitrate_bps,
    Microsoft::WRL::ComPtr<IMFMediaType>* media_type) {
  if (!media_type) return E_POINTER;
  media_type->Reset();

  HRESULT hr = MFCreateMediaType(media_type->ReleaseAndGetAddressOf());
  if (FAILED(hr)) return hr;
  IMFMediaType* type = media_type->Get();

  hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (FAILED(hr)) return hr;
  hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  if (FAILED(hr)) return hr;
  hr = type->SetUINT32(MF_MT_AVG_BITRATE, bitrate_bps);
  if (FAILED(hr)) return hr;
  hr = MFSetAttributeSize(type, MF_MT_FRAME_SIZE, width, height);
  if (FAILED(hr)) return hr;
  hr = MFSetAttributeRatio(type, MF_MT_FRAME_RATE, fps, 1);
  if (FAILED(hr)) return hr;
  hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  if (FAILED(hr)) return hr;
  hr = type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
  if (FAILED(hr)) return hr;
  return MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
}

HRESULT BuildNv12InputMediaType(
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    Microsoft::WRL::ComPtr<IMFMediaType>* media_type) {
  if (!media_type) return E_POINTER;
  media_type->Reset();

  HRESULT hr = MFCreateMediaType(media_type->ReleaseAndGetAddressOf());
  if (FAILED(hr)) return hr;
  IMFMediaType* type = media_type->Get();

  hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (FAILED(hr)) return hr;
  hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  if (FAILED(hr)) return hr;
  hr = MFSetAttributeSize(type, MF_MT_FRAME_SIZE, width, height);
  if (FAILED(hr)) return hr;
  hr = MFSetAttributeRatio(type, MF_MT_FRAME_RATE, fps, 1);
  if (FAILED(hr)) return hr;
  hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  if (FAILED(hr)) return hr;
  return MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
}

bool TryActivateHardwareEncoderCandidates(
    IMFActivate** activations,
    UINT32 activation_count,
    IMFDXGIDeviceManager* device_manager,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    uint32_t bitrate_bps,
    bool adapter_matched,
    ActivatedEncoderCandidate* selected) {
  if (!activations || activation_count == 0 || !device_manager || !selected ||
      width == 0 || height == 0 || fps == 0 || bitrate_bps == 0) {
    return false;
  }

  for (UINT32 i = 0; i < activation_count; ++i) {
    IMFActivate* activation = activations[i];
    if (!activation) continue;

    const std::wstring name = ReadActivationName(activation);
    const wchar_t* source = adapter_matched ? L"capture-adapter" : L"global";
    Microsoft::WRL::ComPtr<IMFTransform> transform;
    HRESULT hr = activation->ActivateObject(IID_PPV_ARGS(&transform));
    if (FAILED(hr) || !transform) {
      std::wcerr << L"H264Encoder: " << source
                 << L" candidate activation failed"
                 << (name.empty() ? L"" : L" for ") << name
                 << L": 0x" << std::hex << hr << L"\n";
      continue;
    }

    auto reject = [&](const wchar_t* stage, HRESULT failure) {
      std::wcerr << L"H264Encoder: " << source
                 << L" candidate preflight failed at " << stage
                 << (name.empty() ? L"" : L" for ") << name
                 << L": 0x" << std::hex << failure << L"\n";
      activation->ShutdownObject();
    };

    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    hr = transform->GetAttributes(&attributes);
    if (FAILED(hr) || !attributes) {
      reject(L"GetAttributes", hr);
      continue;
    }

    UINT32 is_async = FALSE;
    if (SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)) && is_async) {
      hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
      if (FAILED(hr)) {
        reject(L"async unlock", hr);
        continue;
      }
    }
    attributes->SetUINT32(MF_LOW_LATENCY, TRUE);

    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> event_generator;
    hr = transform.As(&event_generator);
    if (FAILED(hr) || !event_generator) {
      reject(L"async event generator", FAILED(hr) ? hr : E_NOINTERFACE);
      continue;
    }

    DWORD input_stream_id = 0;
    DWORD output_stream_id = 0;
    hr = transform->GetStreamIDs(1, &input_stream_id, 1, &output_stream_id);
    if (hr == E_NOTIMPL) {
      input_stream_id = 0;
      output_stream_id = 0;
      hr = S_OK;
    }
    if (FAILED(hr)) {
      reject(L"GetStreamIDs", hr);
      continue;
    }

    UINT32 d3d11_aware = FALSE;
    hr = attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d11_aware);
    if (FAILED(hr) || !d3d11_aware) {
      std::wcerr << L"H264Encoder: " << source
                 << L" candidate is not D3D11-aware; skipping zero-copy path"
                 << (name.empty() ? L"" : L" for ") << name << L"\n";
      activation->ShutdownObject();
      continue;
    }

    hr = transform->ProcessMessage(
        MFT_MESSAGE_SET_D3D_MANAGER,
        reinterpret_cast<ULONG_PTR>(device_manager));
    if (FAILED(hr)) {
      reject(L"D3D manager", hr);
      continue;
    }

    Microsoft::WRL::ComPtr<ICodecAPI> codec_api;
    transform.As(&codec_api);
    ConfigureCandidateCodecApi(codec_api.Get(), fps, bitrate_bps);

    Microsoft::WRL::ComPtr<IMFMediaType> output_type;
    hr = BuildH264OutputMediaType(width, height, fps, bitrate_bps, &output_type);
    if (FAILED(hr)) {
      reject(L"H264 output media type", hr);
      continue;
    }
    hr = transform->SetOutputType(output_stream_id, output_type.Get(), 0);
    if (FAILED(hr)) {
      reject(L"SetOutputType", hr);
      continue;
    }

    Microsoft::WRL::ComPtr<IMFMediaType> input_type;
    hr = BuildNv12InputMediaType(width, height, fps, &input_type);
    if (FAILED(hr)) {
      reject(L"NV12 input media type", hr);
      continue;
    }
    hr = transform->SetInputType(input_stream_id, input_type.Get(), 0);
    if (FAILED(hr)) {
      reject(L"SetInputType(NV12)", hr);
      continue;
    }

    MFT_OUTPUT_STREAM_INFO output_stream_info{};
    hr = transform->GetOutputStreamInfo(output_stream_id, &output_stream_info);
    if (FAILED(hr)) {
      reject(L"GetOutputStreamInfo", hr);
      continue;
    }

    transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
      reject(L"BEGIN_STREAMING", hr);
      continue;
    }
    hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) {
      reject(L"START_OF_STREAM", hr);
      continue;
    }

    selected->transform = transform;
    selected->event_generator = event_generator;
    selected->codec_api = codec_api;
    selected->input_stream_id = input_stream_id;
    selected->output_stream_id = output_stream_id;
    selected->output_stream_info = output_stream_info;
    selected->name = name;
    selected->adapter_matched = adapter_matched;
    return true;
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
  ActivatedEncoderCandidate selected_encoder;
  bool selected_encoder_ready = false;

  LUID adapter_luid{};
  if (TryGetAdapterLuid(device, &adapter_luid)) {
    IMFActivate** adapter_activations = nullptr;
    UINT32 adapter_activation_count = 0;
    hr = EnumerateAdapterHardwareH264Encoders(
        adapter_luid,
        &adapter_activations,
        &adapter_activation_count);
    if (SUCCEEDED(hr) && adapter_activation_count > 0 && adapter_activations) {
      selected_encoder_ready = TryActivateHardwareEncoderCandidates(
          adapter_activations,
          adapter_activation_count,
          device_manager_.Get(),
          width,
          height,
          fps,
          bitrate_bps,
          true,
          &selected_encoder);
    }
    ReleaseActivations(adapter_activations, adapter_activation_count);
  }

  if (!selected_encoder_ready) {
    IMFActivate** global_activations = nullptr;
    UINT32 global_activation_count = 0;
    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr,
        &output_info,
        &global_activations,
        &global_activation_count);
    if (SUCCEEDED(hr) && global_activation_count > 0 && global_activations) {
      selected_encoder_ready = TryActivateHardwareEncoderCandidates(
          global_activations,
          global_activation_count,
          device_manager_.Get(),
          width,
          height,
          fps,
          bitrate_bps,
          false,
          &selected_encoder);
    }
    ReleaseActivations(global_activations, global_activation_count);
  }

  if (!selected_encoder_ready || !selected_encoder.transform) {
    std::wcerr << L"H264Encoder: no usable hardware H264 Media Foundation encoder found\n";
    Reset();
    return false;
  }

  transform_ = selected_encoder.transform;
  event_generator_ = selected_encoder.event_generator;
  codec_api_ = selected_encoder.codec_api;
  input_stream_id_ = selected_encoder.input_stream_id;
  output_stream_id_ = selected_encoder.output_stream_id;
  output_stream_info_ = selected_encoder.output_stream_info;
  encoder_name_ = selected_encoder.name;
  const bool adapter_matched_encoder = selected_encoder.adapter_matched;

  width_ = width;
  height_ = height;
  fps_ = fps;
  bitrate_bps_ = bitrate_bps;
  frame_duration100ns_ = 10'000'000ULL / fps;
  CacheSequenceHeader();

  std::wcout << L"H264 hardware encoder: "
             << (encoder_name_.empty() ? L"<unknown>" : encoder_name_)
             << (adapter_matched_encoder ? L" [capture-adapter match]" : L" [global fallback]")
             << L", " << width << L"x" << height << L" @ " << fps
             << L" fps, " << (bitrate_bps / 1'000'000.0) << L" Mbps\n";
  return true;
}

bool H264Encoder::CacheSequenceHeader() {
  sequence_header_.clear();
  if (!transform_) return false;

  Microsoft::WRL::ComPtr<IMFMediaType> current;
  if (FAILED(transform_->GetOutputCurrentType(output_stream_id_, &current))) return false;

  UINT32 size = 0;
  if (FAILED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) || size == 0) return false;

  std::vector<uint8_t> raw_header(size);
  UINT32 written = 0;
  if (FAILED(current->GetBlob(
          MF_MT_MPEG_SEQUENCE_HEADER,
          raw_header.data(),
          size,
          &written))) {
    return false;
  }
  raw_header.resize(written);
  if (!NormalizeH264SequenceHeader(
          raw_header.data(),
          raw_header.size(),
          &sequence_header_,
          &nal_length_size_,
          &access_units_length_prefixed_)) {
    sequence_header_.clear();
    return false;
  }

  framing_known_ = true;
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

  const auto deadline = std::chrono::steady_clock::now() + kEncoderEventTimeout;
  for (;;) {
    Microsoft::WRL::ComPtr<IMFMediaEvent> event;
    const HRESULT hr = event_generator_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
    if (hr == MF_E_NO_EVENTS_AVAILABLE) {
      if (std::chrono::steady_clock::now() >= deadline) {
        std::wcerr << L"H264Encoder: timed out waiting for Media Foundation event "
                   << static_cast<unsigned long>(wanted) << L"\n";
        return false;
      }
      std::this_thread::sleep_for(kEncoderEventPollInterval);
      continue;
    }
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

  std::vector<uint8_t> raw(bytes, bytes + current_length);
  contiguous->Unlock();

  // Some MFTs expose MF_MT_MPEG_SEQUENCE_HEADER only after streaming starts.
  if (!framing_known_) CacheSequenceHeader();

  std::vector<uint8_t> normalized;
  const bool prefer_length_prefixed = framing_known_
      ? access_units_length_prefixed_
      : true;
  if (!NormalizeH264AccessUnit(
          raw.data(),
          raw.size(),
          nal_length_size_,
          prefer_length_prefixed,
          &normalized)) {
    std::wcerr << L"H264Encoder: output is neither valid Annex-B nor a supported length-prefixed access unit\n";
    return false;
  }
  output->bytes.swap(normalized);

  if (output->keyframe && !sequence_header_.empty() &&
      !H264AnnexBHasParameterSet(output->bytes)) {
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

  auto recover_stream = [this]() {
    if (!transform_) return;
    transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    need_input_pending_ = false;
    have_output_pending_ = false;
    const HRESULT restart = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(restart)) {
      std::wcerr << L"H264Encoder: failed to restart stream after encoder stall: 0x"
                 << std::hex << restart << L"\n";
    }
    RequestKeyframe();
  };

  if (!WaitForEvent(METransformNeedInput)) {
    recover_stream();
    return false;
  }
  if (!SubmitInput(nv12_texture, timestamp100ns)) {
    recover_stream();
    return false;
  }
  if (!WaitForEvent(METransformHaveOutput)) {
    recover_stream();
    return false;
  }
  if (!ReadOutput(output)) {
    recover_stream();
    return false;
  }
  return true;
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
  nal_length_size_ = 4;
  access_units_length_prefixed_ = false;
  framing_known_ = false;
  encoder_name_.clear();
}

}  // namespace desklink