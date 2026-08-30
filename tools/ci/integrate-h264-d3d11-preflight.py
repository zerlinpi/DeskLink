from pathlib import Path

path = Path("apps/windows-agent/src/h264_encoder.cpp")
text = path.read_text(encoding="utf-8")

if "MF_SA_D3D11_AWARE" in text and "candidate preflight failed" in text:
    raise SystemExit("H264 candidate preflight is already integrated")
if "TryActivateHardwareEncoderCandidates" not in text:
    raise SystemExit("activation candidate fallback must be integrated first")

old_struct = '''struct ActivatedEncoderCandidate {
  Microsoft::WRL::ComPtr<IMFTransform> transform;
  std::wstring name;
  bool adapter_matched{false};
};
'''
new_struct = '''struct ActivatedEncoderCandidate {
  Microsoft::WRL::ComPtr<IMFTransform> transform;
  Microsoft::WRL::ComPtr<IMFMediaEventGenerator> event_generator;
  Microsoft::WRL::ComPtr<ICodecAPI> codec_api;
  DWORD input_stream_id{0};
  DWORD output_stream_id{0};
  std::wstring name;
  bool adapter_matched{false};
};
'''
if text.count(old_struct) != 1:
    raise SystemExit(f"candidate struct anchor count={text.count(old_struct)}")
text = text.replace(old_struct, new_struct, 1)

helper_start = text.index("bool TryActivateHardwareEncoderCandidates(\n")
helper_end = text.index("\n}\n\n}  // namespace\n", helper_start) + len("\n}\n")
new_helper = r'''bool TryActivateHardwareEncoderCandidates(
    IMFActivate** activations,
    UINT32 activation_count,
    IMFDXGIDeviceManager* device_manager,
    bool adapter_matched,
    ActivatedEncoderCandidate* selected) {
  if (!activations || activation_count == 0 || !device_manager || !selected) return false;

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

    selected->transform = transform;
    selected->event_generator = event_generator;
    selected->codec_api = codec_api;
    selected->input_stream_id = input_stream_id;
    selected->output_stream_id = output_stream_id;
    selected->name = name;
    selected->adapter_matched = adapter_matched;
    return true;
  }
  return false;
}
'''
text = text[:helper_start] + new_helper + text[helper_end:]

text = text.replace(
    "          adapter_activation_count,\n          true,\n          &selected_encoder);",
    "          adapter_activation_count,\n          device_manager_.Get(),\n          true,\n          &selected_encoder);",
    1,
)
text = text.replace(
    "          global_activation_count,\n          false,\n          &selected_encoder);",
    "          global_activation_count,\n          device_manager_.Get(),\n          false,\n          &selected_encoder);",
    1,
)
if text.count("device_manager_.Get(),\n          true") != 1 or text.count("device_manager_.Get(),\n          false") != 1:
    raise SystemExit("candidate call integration anchors were not replaced exactly once")

post_select_start = text.index("  transform_ = selected_encoder.transform;\n")
post_select_end = text.index("  width_ = width;\n", post_select_start)
post_select = '''  transform_ = selected_encoder.transform;
  event_generator_ = selected_encoder.event_generator;
  codec_api_ = selected_encoder.codec_api;
  input_stream_id_ = selected_encoder.input_stream_id;
  output_stream_id_ = selected_encoder.output_stream_id;
  encoder_name_ = selected_encoder.name;
  const bool adapter_matched_encoder = selected_encoder.adapter_matched;

'''
text = text[:post_select_start] + post_select + text[post_select_end:]

path.write_text(text, encoding="utf-8")
