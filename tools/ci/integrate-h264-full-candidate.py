from pathlib import Path

cpp_path = Path("apps/windows-agent/src/h264_encoder.cpp")
header_path = Path("apps/windows-agent/src/h264_encoder.h")
cpp = cpp_path.read_text(encoding="utf-8")
header = header_path.read_text(encoding="utf-8")

if "BuildH264OutputMediaType" in cpp:
    raise SystemExit("full H264 candidate configuration transaction is already integrated")
if "TryActivateHardwareEncoderCandidates" not in cpp or "MF_SA_D3D11_AWARE" not in cpp:
    raise SystemExit("D3D11 candidate preflight must be integrated first")

old_decl = "  bool ConfigureCodecApi(uint32_t bitrate_bps);\n"
if header.count(old_decl) != 1:
    raise SystemExit(f"ConfigureCodecApi declaration anchor count={header.count(old_decl)}")
header = header.replace(old_decl, "", 1)

old_struct_tail = '''  DWORD input_stream_id{0};
  DWORD output_stream_id{0};
  std::wstring name;
'''
new_struct_tail = '''  DWORD input_stream_id{0};
  DWORD output_stream_id{0};
  MFT_OUTPUT_STREAM_INFO output_stream_info{};
  std::wstring name;
'''
if cpp.count(old_struct_tail) != 1:
    raise SystemExit(f"candidate struct tail anchor count={cpp.count(old_struct_tail)}")
cpp = cpp.replace(old_struct_tail, new_struct_tail, 1)

function_marker = "bool TryActivateHardwareEncoderCandidates(\n"
if cpp.count(function_marker) != 1:
    raise SystemExit(f"candidate function marker count={cpp.count(function_marker)}")

helpers = r'''bool ConfigureCandidateCodecApi(ICodecAPI* codec, uint32_t fps, uint32_t bitrate_bps) {
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

'''
cpp = cpp.replace(function_marker, helpers + function_marker, 1)

old_signature = '''bool TryActivateHardwareEncoderCandidates(
    IMFActivate** activations,
    UINT32 activation_count,
    IMFDXGIDeviceManager* device_manager,
    bool adapter_matched,
    ActivatedEncoderCandidate* selected) {
  if (!activations || activation_count == 0 || !device_manager || !selected) return false;
'''
new_signature = '''bool TryActivateHardwareEncoderCandidates(
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
'''
if cpp.count(old_signature) != 1:
    raise SystemExit(f"candidate signature anchor count={cpp.count(old_signature)}")
cpp = cpp.replace(old_signature, new_signature, 1)

old_commit = '''    Microsoft::WRL::ComPtr<ICodecAPI> codec_api;
    transform.As(&codec_api);

    selected->transform = transform;
    selected->event_generator = event_generator;
    selected->codec_api = codec_api;
    selected->input_stream_id = input_stream_id;
    selected->output_stream_id = output_stream_id;
    selected->name = name;
    selected->adapter_matched = adapter_matched;
    return true;
'''
new_commit = '''    Microsoft::WRL::ComPtr<ICodecAPI> codec_api;
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
'''
if cpp.count(old_commit) != 1:
    raise SystemExit(f"candidate commit anchor count={cpp.count(old_commit)}")
cpp = cpp.replace(old_commit, new_commit, 1)

old_adapter_call = '''          adapter_activation_count,
          device_manager_.Get(),
          true,
          &selected_encoder);'''
new_adapter_call = '''          adapter_activation_count,
          device_manager_.Get(),
          width,
          height,
          fps,
          bitrate_bps,
          true,
          &selected_encoder);'''
if cpp.count(old_adapter_call) != 1:
    raise SystemExit(f"adapter call anchor count={cpp.count(old_adapter_call)}")
cpp = cpp.replace(old_adapter_call, new_adapter_call, 1)

old_global_call = '''          global_activation_count,
          device_manager_.Get(),
          false,
          &selected_encoder);'''
new_global_call = '''          global_activation_count,
          device_manager_.Get(),
          width,
          height,
          fps,
          bitrate_bps,
          false,
          &selected_encoder);'''
if cpp.count(old_global_call) != 1:
    raise SystemExit(f"global call anchor count={cpp.count(old_global_call)}")
cpp = cpp.replace(old_global_call, new_global_call, 1)

selection_start = cpp.index("  transform_ = selected_encoder.transform;\n")
log_marker = '  std::wcout << L"H264 hardware encoder: "\n'
selection_end = cpp.index(log_marker, selection_start)
replacement = '''  transform_ = selected_encoder.transform;
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

'''
cpp = cpp[:selection_start] + replacement + cpp[selection_end:]

method_start = cpp.find("bool H264Encoder::ConfigureCodecApi(uint32_t bitrate_bps) {")
if method_start == -1:
    raise SystemExit("ConfigureCodecApi implementation was not found")
method_end_marker = "bool H264Encoder::CacheSequenceHeader() {"
method_end = cpp.find(method_end_marker, method_start)
if method_end == -1:
    raise SystemExit("CacheSequenceHeader marker was not found after ConfigureCodecApi")
cpp = cpp[:method_start] + cpp[method_end:]

required = [
    "BuildH264OutputMediaType",
    "BuildNv12InputMediaType",
    "ConfigureCandidateCodecApi",
    "reject(L\"SetOutputType\"",
    "reject(L\"SetInputType(NV12)\"",
    "reject(L\"BEGIN_STREAMING\"",
    "reject(L\"START_OF_STREAM\"",
    "output_stream_info_ = selected_encoder.output_stream_info",
]
for needle in required:
    if needle not in cpp:
        raise SystemExit(f"missing full-transaction invariant: {needle}")
if "H264Encoder::ConfigureCodecApi" in cpp or "ConfigureCodecApi(uint32_t bitrate_bps)" in header:
    raise SystemExit("legacy member codec configuration path remains")

cpp_path.write_text(cpp, encoding="utf-8")
header_path.write_text(header, encoding="utf-8")
