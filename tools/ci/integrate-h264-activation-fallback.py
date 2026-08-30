from pathlib import Path

path = Path("apps/windows-agent/src/h264_encoder.cpp")
text = path.read_text(encoding="utf-8")

if "TryActivateHardwareEncoderCandidates" in text:
    raise SystemExit("candidate activation fallback is already integrated")

namespace_marker = "\n}\n\n}  // namespace\n\nbool H264Encoder::Initialize(\n"
if text.count(namespace_marker) != 1:
    raise SystemExit(f"namespace insertion marker count={text.count(namespace_marker)}")

helper = r'''
}

struct ActivatedEncoderCandidate {
  Microsoft::WRL::ComPtr<IMFTransform> transform;
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

bool TryActivateHardwareEncoderCandidates(
    IMFActivate** activations,
    UINT32 activation_count,
    bool adapter_matched,
    ActivatedEncoderCandidate* selected) {
  if (!activations || activation_count == 0 || !selected) return false;

  for (UINT32 i = 0; i < activation_count; ++i) {
    IMFActivate* activation = activations[i];
    if (!activation) continue;

    const std::wstring name = ReadActivationName(activation);
    Microsoft::WRL::ComPtr<IMFTransform> transform;
    const HRESULT hr = activation->ActivateObject(IID_PPV_ARGS(&transform));
    if (FAILED(hr) || !transform) {
      std::wcerr << L"H264Encoder: "
                 << (adapter_matched ? L"capture-adapter" : L"global")
                 << L" candidate activation failed"
                 << (name.empty() ? L"" : L" for ")
                 << name
                 << L": 0x" << std::hex << hr << L"\n";
      continue;
    }

    selected->transform = transform;
    selected->name = name;
    selected->adapter_matched = adapter_matched;
    return true;
  }
  return false;
}

}  // namespace

bool H264Encoder::Initialize(
'''
text = text.replace(namespace_marker, "\n" + helper, 1)

initialize_start = text.index("bool H264Encoder::Initialize(")
selection_start = text.index(
    "  MFT_REGISTER_TYPE_INFO output_info{MFMediaType_Video, MFVideoFormat_H264};\n",
    initialize_start,
)
selection_end = text.index(
    "  Microsoft::WRL::ComPtr<IMFAttributes> attributes;\n",
    selection_start,
)

replacement = r'''  MFT_REGISTER_TYPE_INFO output_info{MFMediaType_Video, MFVideoFormat_H264};
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
  encoder_name_ = selected_encoder.name;
  const bool adapter_matched_encoder = selected_encoder.adapter_matched;

'''
text = text[:selection_start] + replacement + text[selection_end:]

path.write_text(text, encoding="utf-8")
