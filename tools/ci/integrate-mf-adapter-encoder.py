from pathlib import Path

path = Path("apps/windows-agent/src/h264_encoder.cpp")
text = path.read_text(encoding="utf-8")

include_anchor = '#include <mfapi.h>\n#include <mferror.h>\n'
include_replacement = '#include <dxgi.h>\n#include <mfapi.h>\n#include <mferror.h>\n#include <mftransform.h>\n'
if text.count(include_anchor) != 1:
    raise SystemExit(f"include anchor count={text.count(include_anchor)}")
text = text.replace(include_anchor, include_replacement, 1)

helper_anchor = '''bool SetCodecBool(ICodecAPI* codec, const GUID& key, bool value) {
  if (!codec) return false;
  VARIANT variant;
  VariantInit(&variant);
  variant.vt = VT_BOOL;
  variant.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
  return SUCCEEDED(codec->SetValue(&key, &variant));
}

}  // namespace
'''
helper_replacement = '''bool SetCodecBool(ICodecAPI* codec, const GUID& key, bool value) {
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

}  // namespace
'''
if text.count(helper_anchor) != 1:
    raise SystemExit(f"helper anchor count={text.count(helper_anchor)}")
text = text.replace(helper_anchor, helper_replacement, 1)

selection_anchor = '''  MFT_REGISTER_TYPE_INFO output_info{MFMediaType_Video, MFVideoFormat_H264};
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
    std::wcerr << L"H264Encoder: no hardware H264 Media Foundation encoder found\\n";
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
'''
selection_replacement = '''  MFT_REGISTER_TYPE_INFO output_info{MFMediaType_Video, MFVideoFormat_H264};
  IMFActivate** activations = nullptr;
  UINT32 activation_count = 0;
  bool adapter_matched_encoder = false;

  LUID adapter_luid{};
  if (TryGetAdapterLuid(device, &adapter_luid)) {
    hr = EnumerateAdapterHardwareH264Encoders(
        adapter_luid,
        &activations,
        &activation_count);
    if (SUCCEEDED(hr) && activation_count > 0 && activations) {
      adapter_matched_encoder = true;
    } else {
      ReleaseActivations(activations, activation_count);
      activations = nullptr;
      activation_count = 0;
    }
  }

  if (!adapter_matched_encoder) {
    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr,
        &output_info,
        &activations,
        &activation_count);
  }

  if (FAILED(hr) || activation_count == 0 || !activations) {
    std::wcerr << L"H264Encoder: no hardware H264 Media Foundation encoder found\\n";
    ReleaseActivations(activations, activation_count);
    Reset();
    return false;
  }

  Microsoft::WRL::ComPtr<IMFActivate> activation;
  activation.Attach(activations[0]);
  activations[0] = nullptr;
  ReleaseActivations(activations, activation_count);
'''
if text.count(selection_anchor) != 1:
    raise SystemExit(f"selection anchor count={text.count(selection_anchor)}")
text = text.replace(selection_anchor, selection_replacement, 1)

log_anchor = '''  std::wcout << L"H264 hardware encoder: "
             << (encoder_name_.empty() ? L"<unknown>" : encoder_name_)
             << L", " << width << L"x" << height << L" @ " << fps
             << L" fps, " << (bitrate_bps / 1'000'000.0) << L" Mbps\\n";
'''
log_replacement = '''  std::wcout << L"H264 hardware encoder: "
             << (encoder_name_.empty() ? L"<unknown>" : encoder_name_)
             << (adapter_matched_encoder ? L" [capture-adapter match]" : L" [global fallback]")
             << L", " << width << L"x" << height << L" @ " << fps
             << L" fps, " << (bitrate_bps / 1'000'000.0) << L" Mbps\\n";
'''
if text.count(log_anchor) != 1:
    raise SystemExit(f"log anchor count={text.count(log_anchor)}")
text = text.replace(log_anchor, log_replacement, 1)

path.write_text(text, encoding="utf-8")
