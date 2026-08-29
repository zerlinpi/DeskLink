#include "gpu_color_converter.h"

#include <dxgi1_2.h>

#include <iostream>

namespace desklink {

bool GpuColorConverter::Initialize(
    ID3D11Device* device,
    uint32_t input_width,
    uint32_t input_height,
    uint32_t output_width,
    uint32_t output_height,
    uint32_t fps) {
  Reset();
  if (!device || input_width == 0 || input_height == 0 ||
      output_width == 0 || output_height == 0 || fps == 0) {
    return false;
  }

  device_ = device;
  device_->GetImmediateContext(&context_);

  HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(&video_device_));
  if (FAILED(hr)) {
    std::wcerr << L"GpuColorConverter: ID3D11VideoDevice unavailable: 0x" << std::hex << hr << L"\n";
    Reset();
    return false;
  }

  hr = context_->QueryInterface(IID_PPV_ARGS(&video_context_));
  if (FAILED(hr)) {
    std::wcerr << L"GpuColorConverter: ID3D11VideoContext unavailable: 0x" << std::hex << hr << L"\n";
    Reset();
    return false;
  }

  D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
  content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  content.InputFrameRate.Numerator = fps;
  content.InputFrameRate.Denominator = 1;
  content.InputWidth = input_width;
  content.InputHeight = input_height;
  content.OutputFrameRate.Numerator = fps;
  content.OutputFrameRate.Denominator = 1;
  content.OutputWidth = output_width;
  content.OutputHeight = output_height;
  content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

  hr = video_device_->CreateVideoProcessorEnumerator(&content, &enumerator_);
  if (FAILED(hr)) {
    std::wcerr << L"GpuColorConverter: CreateVideoProcessorEnumerator failed: 0x" << std::hex << hr << L"\n";
    Reset();
    return false;
  }

  UINT format_flags = 0;
  hr = enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &format_flags);
  if (FAILED(hr) || (format_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
    std::wcerr << L"GpuColorConverter: GPU video processor cannot output NV12\n";
    Reset();
    return false;
  }

  hr = video_device_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_);
  if (FAILED(hr)) {
    std::wcerr << L"GpuColorConverter: CreateVideoProcessor failed: 0x" << std::hex << hr << L"\n";
    Reset();
    return false;
  }

  D3D11_TEXTURE2D_DESC surface{};
  surface.Width = output_width;
  surface.Height = output_height;
  surface.MipLevels = 1;
  surface.ArraySize = 1;
  surface.Format = DXGI_FORMAT_NV12;
  surface.SampleDesc.Count = 1;
  surface.Usage = D3D11_USAGE_DEFAULT;
  surface.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  hr = device_->CreateTexture2D(&surface, nullptr, &nv12_texture_);
  if (FAILED(hr)) {
    std::wcerr << L"GpuColorConverter: NV12 texture creation failed: 0x" << std::hex << hr << L"\n";
    Reset();
    return false;
  }

  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
  output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  output_desc.Texture2D.MipSlice = 0;
  hr = video_device_->CreateVideoProcessorOutputView(
      nv12_texture_.Get(),
      enumerator_.Get(),
      &output_desc,
      &output_view_);
  if (FAILED(hr)) {
    std::wcerr << L"GpuColorConverter: output view creation failed: 0x" << std::hex << hr << L"\n";
    Reset();
    return false;
  }

  const RECT source_rect{0, 0, static_cast<LONG>(input_width), static_cast<LONG>(input_height)};
  const RECT target_rect{0, 0, static_cast<LONG>(output_width), static_cast<LONG>(output_height)};
  video_context_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &target_rect);
  video_context_->VideoProcessorSetStreamFrameFormat(
      processor_.Get(),
      0,
      D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &source_rect);
  video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &target_rect);
  video_context_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);

  input_width_ = input_width;
  input_height_ = input_height;
  output_width_ = output_width;
  output_height_ = output_height;
  return true;
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> GpuColorConverter::Convert(
    ID3D11Texture2D* bgra_texture) {
  if (!bgra_texture || !processor_ || !output_view_) return {};

  D3D11_TEXTURE2D_DESC source_desc{};
  bgra_texture->GetDesc(&source_desc);
  if (source_desc.Width != input_width_ || source_desc.Height != input_height_) {
    std::wcerr << L"GpuColorConverter: source size changed; reinitialize converter\n";
    return {};
  }

  UINT input_format_flags = 0;
  HRESULT hr = enumerator_->CheckVideoProcessorFormat(source_desc.Format, &input_format_flags);
  if (FAILED(hr) || (input_format_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) {
    std::wcerr << L"GpuColorConverter: desktop format is not accepted by video processor\n";
    return {};
  }

  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
  input_desc.FourCC = 0;
  input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  input_desc.Texture2D.MipSlice = 0;
  input_desc.Texture2D.ArraySlice = 0;

  Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view;
  hr = video_device_->CreateVideoProcessorInputView(
      bgra_texture,
      enumerator_.Get(),
      &input_desc,
      &input_view);
  if (FAILED(hr)) {
    std::wcerr << L"GpuColorConverter: input view creation failed: 0x" << std::hex << hr << L"\n";
    return {};
  }

  D3D11_VIDEO_PROCESSOR_STREAM stream{};
  stream.Enable = TRUE;
  stream.OutputIndex = 0;
  stream.InputFrameOrField = 0;
  stream.PastFrames = 0;
  stream.FutureFrames = 0;
  stream.pInputSurface = input_view.Get();

  hr = video_context_->VideoProcessorBlt(
      processor_.Get(),
      output_view_.Get(),
      0,
      1,
      &stream);
  if (FAILED(hr)) {
    std::wcerr << L"GpuColorConverter: VideoProcessorBlt failed: 0x" << std::hex << hr << L"\n";
    return {};
  }

  return nv12_texture_;
}

void GpuColorConverter::Reset() {
  output_view_.Reset();
  nv12_texture_.Reset();
  processor_.Reset();
  enumerator_.Reset();
  video_context_.Reset();
  video_device_.Reset();
  context_.Reset();
  device_.Reset();
  input_width_ = 0;
  input_height_ = 0;
  output_width_ = 0;
  output_height_ = 0;
}

}  // namespace desklink
