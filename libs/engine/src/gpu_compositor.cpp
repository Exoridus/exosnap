#include "gpu_compositor.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace exosnap::engine {
namespace {

void SetHResultError(std::string& err, const char* what, HRESULT hr) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s failed 0x%08lX", what, static_cast<unsigned long>(hr));
    err = buf;
}

// scRGB reference white (channel 1.0). The HDR overlay reference white used to
// place SDR overlay sprites on the HDR timeline (a diffuse-white paper level)
// is caller-supplied to Init (overlay_ref_white_nits_), defaulting to
// kDefaultSdrWhiteLevelNits.
constexpr float kScrgbReferenceWhiteNits = 80.0f;

} // namespace

bool GpuCompositor::Init(ID3D11Device* device, ID3D11DeviceContext* context, UINT width, UINT height, std::string& err,
                         DXGI_FORMAT render_format, float overlay_reference_white_nits) {
    if (device == nullptr || context == nullptr || width == 0 || height == 0) {
        err = "GpuCompositor::Init invalid arguments";
        return false;
    }
    if (render_format != DXGI_FORMAT_B8G8R8A8_UNORM && render_format != DXGI_FORMAT_R10G10B10A2_UNORM &&
        render_format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
        err = "GpuCompositor::Init unsupported render format";
        return false;
    }

    device_ = device;
    context_ = context;
    width_ = width;
    height_ = height;
    render_format_ = render_format;
    // FP16 render target = the native HDR10 path: the background is linear scRGB,
    // so overlay sprites are composited in linear light (see the pixel shader).
    hdr_linear_ = (render_format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    overlay_ref_white_nits_ = EffectiveOverlayReferenceWhiteNits(overlay_reference_white_nits);

    winrt::com_ptr<ID3DBlob> vs_blob;
    winrt::com_ptr<ID3DBlob> ps_blob;
    winrt::com_ptr<ID3DBlob> error_blob;

    HRESULT hr =
        D3DCompile(kOverlayVertexShaderSrc, std::strlen(kOverlayVertexShaderSrc), "gpu_compositor_vs", nullptr, nullptr,
                   "main", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, vs_blob.put(), error_blob.put());
    if (FAILED(hr)) {
        SetHResultError(err, "D3DCompile(vertex shader)", hr);
        return false;
    }

    error_blob = nullptr;
    hr = D3DCompile(kOverlayPixelShaderSrc, std::strlen(kOverlayPixelShaderSrc), "gpu_compositor_ps", nullptr, nullptr,
                    "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, ps_blob.put(), error_blob.put());
    if (FAILED(hr)) {
        SetHResultError(err, "D3DCompile(pixel shader)", hr);
        return false;
    }

    hr = device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr,
                                     vertex_shader_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateVertexShader", hr);
        return false;
    }
    hr =
        device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, pixel_shader_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreatePixelShader", hr);
        return false;
    }

    D3D11_TEXTURE2D_DESC tex_desc{};
    tex_desc.Width = width_;
    tex_desc.Height = height_;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = render_format_; // matches the capture source's frame format
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    // SHADER_RESOURCE so the composited surface can also feed the native HDR10
    // PQ pass as an input; harmless for the SDR VideoProcessor path.
    tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    hr = device_->CreateTexture2D(&tex_desc, nullptr, composite_tex_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateTexture2D(composite)", hr);
        return false;
    }
    hr = device_->CreateRenderTargetView(composite_tex_.get(), nullptr, composite_rtv_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateRenderTargetView(composite)", hr);
        return false;
    }

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = device_->CreateSamplerState(&sampler_desc, sampler_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateSamplerState", hr);
        return false;
    }

    D3D11_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = device_->CreateBlendState(&blend_desc, blend_state_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateBlendState", hr);
        return false;
    }

    D3D11_BUFFER_DESC const_desc{};
    const_desc.ByteWidth = sizeof(OverlayPixelConstants);
    const_desc.Usage = D3D11_USAGE_DEFAULT;
    const_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    hr = device_->CreateBuffer(&const_desc, nullptr, constants_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateBuffer(constants)", hr);
        return false;
    }

    return true;
}

bool GpuCompositor::BeginFrame(ID3D11Texture2D* background, std::string& err) {
    if (background == nullptr || composite_tex_ == nullptr || context_ == nullptr) {
        err = "GpuCompositor::BeginFrame called before Init or without background";
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    background->GetDesc(&desc);
    if (desc.Width != width_ || desc.Height != height_ || desc.Format != render_format_) {
        err = "GpuCompositor::BeginFrame background dimensions/format mismatch";
        return false;
    }

    context_->CopyResource(composite_tex_.get(), background);
    return true;
}

bool GpuCompositor::DrawWebcam(const uint8_t* bgra, int width, int height, const WebcamPixelRect& rect, bool mirror,
                               const ChromaKeyParams& chroma, std::string& err, float opacity) {
    if (!UploadTexture(webcam_tex_, bgra, width, height, static_cast<UINT>(width * 4), err)) {
        return false;
    }
    return DrawTexture(webcam_tex_.srv.get(), rect, mirror, chroma, true, opacity, err);
}

bool GpuCompositor::DrawCursor(const uint8_t* bgra, int width, int height, const WebcamPixelRect& rect,
                               std::string& err) {
    if (!UploadTexture(cursor_tex_, bgra, width, height, static_cast<UINT>(width * 4), err)) {
        return false;
    }

    ChromaKeyParams chroma;
    return DrawTexture(cursor_tex_.srv.get(), rect, false, chroma, false, 1.0f, err);
}

bool GpuCompositor::UploadTexture(TextureResource& resource, const uint8_t* bgra, int width, int height, UINT row_pitch,
                                  std::string& err) {
    if (device_ == nullptr || context_ == nullptr || bgra == nullptr || width <= 0 || height <= 0) {
        err = "GpuCompositor::UploadTexture invalid arguments";
        return false;
    }

    const UINT tex_w = static_cast<UINT>(width);
    const UINT tex_h = static_cast<UINT>(height);
    if (!resource.texture || resource.width != tex_w || resource.height != tex_h) {
        resource.texture = nullptr;
        resource.srv = nullptr;
        resource.width = 0;
        resource.height = 0;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = tex_w;
        desc.Height = tex_h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, resource.texture.put());
        if (FAILED(hr)) {
            SetHResultError(err, "CreateTexture2D(upload)", hr);
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = desc.Format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;

        hr = device_->CreateShaderResourceView(resource.texture.get(), &srv_desc, resource.srv.put());
        if (FAILED(hr)) {
            SetHResultError(err, "CreateShaderResourceView(upload)", hr);
            resource.texture = nullptr;
            return false;
        }

        resource.width = tex_w;
        resource.height = tex_h;
    }

    context_->UpdateSubresource(resource.texture.get(), 0, nullptr, bgra, row_pitch, 0);
    return true;
}

bool GpuCompositor::DrawTexture(ID3D11ShaderResourceView* srv, const WebcamPixelRect& rect, bool mirror,
                                const ChromaKeyParams& chroma, bool force_opaque, float opacity, std::string& err) {
    if (srv == nullptr || context_ == nullptr || composite_rtv_ == nullptr || !rect.IsValid()) {
        err = "GpuCompositor::DrawTexture invalid arguments";
        return false;
    }
    if (rect.x < 0 || rect.y < 0 || rect.x + rect.w > static_cast<int>(width_) ||
        rect.y + rect.h > static_cast<int>(height_)) {
        err = "GpuCompositor::DrawTexture rect outside target";
        return false;
    }

    // HDR-linear compositing (native HDR10): decode overlay sprites to linear and
    // scale to the configured overlay reference white (in scRGB, where 1.0 = 80
    // nits) — the display's SDR content brightness when known, else the 203
    // cd/m^2 fallback (see GpuCompositor::Init / EffectiveOverlayReferenceWhiteNits).
    const OverlayPixelConstants pc = MakeOverlayPixelConstants(chroma, mirror, force_opaque, opacity, hdr_linear_,
                                                               overlay_ref_white_nits_ / kScrgbReferenceWhiteNits);
    context_->UpdateSubresource(constants_.get(), 0, nullptr, &pc, 0, 0);

    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = static_cast<float>(rect.x);
    viewport.TopLeftY = static_cast<float>(rect.y);
    viewport.Width = static_cast<float>(rect.w);
    viewport.Height = static_cast<float>(rect.h);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    float blend_factor[4] = {};
    ID3D11RenderTargetView* rtv = composite_rtv_.get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->OMSetBlendState(blend_state_.get(), blend_factor, 0xffffffff);
    context_->RSSetViewports(1, &viewport);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_shader_.get(), nullptr, 0);
    context_->PSSetShader(pixel_shader_.get(), nullptr, 0);
    ID3D11SamplerState* sampler = sampler_.get();
    ID3D11Buffer* constants = constants_.get();
    context_->PSSetSamplers(0, 1, &sampler);
    context_->PSSetConstantBuffers(0, 1, &constants);
    context_->PSSetShaderResources(0, 1, &srv);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* null_srv = nullptr;
    context_->PSSetShaderResources(0, 1, &null_srv);
    return true;
}

} // namespace exosnap::engine
