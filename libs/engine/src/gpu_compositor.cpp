#include <cstdint>
#include <exosnap/engine/gpu_compositor.h>
#include <exosnap/engine/gpu_surface_inventory.h>

#include "exosnap/engine/overlay_shader.h"
#include "exosnap/engine/overlay_sprites.h"
#include "exosnap/engine/recorder_session.h"
#include "exosnap/engine/sdr_white_level.h"
#include "exosnap/engine/webcam_placement.h"
#include "gpu_yuv_constants.h"
#include "measured_shader_compile.h"

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <winrt/base.h>

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

// Camera YUV -> gamma-encoded RGB at the camera's own resolution, one output
// pixel per input pixel: integer Load(), no filtering, so the overlay pass that
// follows samples exactly what a CPU conversion would have produced. Chroma is
// taken from the sample covering the pixel (no interpolation), which is what
// the camera's own 4:2:0 / 4:2:2 siting gives a pixel without guessing.
constexpr char kWebcamYuvConstantsHlsl[] = R"(
cbuffer YuvToRgbConstants : register(b0) {
    float c_rv, c_gu, c_gv, c_bu;
    float y_scale;
    float y_off, c_off, unused;
};

float4 YuvToRgb(float y, float u, float v) {
    float yn = (y - y_off) * y_scale;
    float un = u - c_off;
    float vn = v - c_off;
    return float4(saturate(yn + c_rv * vn), saturate(yn - c_gu * un - c_gv * vn), saturate(yn + c_bu * un), 1.0f);
}
)";

constexpr char kWebcamNv12PixelShaderSrc[] = R"(
Texture2D<uint> LumaPlane : register(t0);
Texture2D<uint2> ChromaPlane : register(t1);

float4 main(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    int2 px = int2(position.xy);
    uint2 chroma = ChromaPlane.Load(int3(px / 2, 0));
    return YuvToRgb(float(LumaPlane.Load(int3(px, 0))), float(chroma.x), float(chroma.y));
}
)";

constexpr char kWebcamYuy2PixelShaderSrc[] = R"(
Texture2D<uint4> PackedPairs : register(t0); // Y0 Cb Y1 Cr per texel

float4 main(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    int2 px = int2(position.xy);
    uint4 pair = PackedPairs.Load(int3(px.x / 2, px.y, 0));
    float y = (px.x & 1) != 0 ? float(pair.z) : float(pair.x);
    return YuvToRgb(y, float(pair.y), float(pair.w));
}
)";

bool CompileWebcamShader(ID3D11Device* device, const char* body, const char* name,
                         winrt::com_ptr<ID3D11PixelShader>& out, std::string& err) {
    const std::string source = std::string(kWebcamYuvConstantsHlsl) + body;
    winrt::com_ptr<ID3DBlob> blob;
    winrt::com_ptr<ID3DBlob> error_blob;
    HRESULT hr = MeasuredD3DCompile(source.c_str(), source.size(), name, nullptr, nullptr, "main", "ps_5_0",
                                    D3DCOMPILE_ENABLE_STRICTNESS, 0, blob.put(), error_blob.put());
    if (FAILED(hr)) {
        SetHResultError(err, name, hr);
        if (error_blob) {
            err += ": ";
            err += static_cast<const char*>(error_blob->GetBufferPointer());
        }
        return false;
    }
    hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreatePixelShader(webcam yuv)", hr);
        return false;
    }
    return true;
}

} // namespace

bool GpuCompositor::Init(ID3D11Device* device, ID3D11DeviceContext* context, UINT width, UINT height, std::string& err,
                         DXGI_FORMAT render_format, float overlay_reference_white_nits) {
    if (device == nullptr || context == nullptr || width == 0 || height == 0) {
        err = "GpuCompositor::Init invalid arguments";
        return false;
    }
    if (render_format != DXGI_FORMAT_B8G8R8A8_UNORM && render_format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        render_format != DXGI_FORMAT_R10G10B10A2_UNORM && render_format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
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
        MeasuredD3DCompile(kOverlayVertexShaderSrc, std::strlen(kOverlayVertexShaderSrc), "gpu_compositor_vs", nullptr,
                           nullptr, "main", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, vs_blob.put(), error_blob.put());
    if (FAILED(hr)) {
        SetHResultError(err, "D3DCompile(vertex shader)", hr);
        return false;
    }

    error_blob = nullptr;
    hr =
        MeasuredD3DCompile(kOverlayPixelShaderSrc, std::strlen(kOverlayPixelShaderSrc), "gpu_compositor_ps", nullptr,
                           nullptr, "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, ps_blob.put(), error_blob.put());
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

    hr = exosnap::engine::CreateTrackedTexture2D(device_, &tex_desc, nullptr, composite_tex_.put(),
                                                 exosnap::engine::GpuSurfaceOwner::Compositor);
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

    // The mask cursor's inverting plane. With a source that is opaque white where
    // the pixel inverts and zero elsewhere:
    //   invert     1 * (1 - dst) + dst * 0 = 1 - dst
    //   elsewhere  0 * (1 - dst) + dst * 1 = dst
    // so the pass needs no clipping and a cursor that mixes inverting with
    // ordinary opaque pixels still composites correctly. Alpha is taken from the
    // destination alone, so an inversion cannot disturb the frame's own alpha.
    D3D11_BLEND_DESC invert_desc{};
    invert_desc.RenderTarget[0].BlendEnable = TRUE;
    invert_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
    invert_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    invert_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    invert_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    invert_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    invert_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    invert_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = device_->CreateBlendState(&invert_desc, invert_blend_state_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateBlendState(invert)", hr);
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

void GpuCompositor::SetOverlayReferenceWhiteNits(float overlay_reference_white_nits) noexcept {
    // Read per DrawTexture into the sprite's constants, so there is nothing to
    // rewrite here -- the next draw picks this up.
    overlay_ref_white_nits_ = EffectiveOverlayReferenceWhiteNits(overlay_reference_white_nits);
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
                               const ChromaKeyParams& chroma, std::string& err, float opacity,
                               std::optional<uint64_t> generation) {
    if (!generation || generation != webcam_generation_ || !webcam_tex_.srv ||
        webcam_tex_.width != static_cast<UINT>(width) || webcam_tex_.height != static_cast<UINT>(height)) {
        if (!UploadTexture(webcam_tex_, bgra, width, height, static_cast<UINT>(width * 4), err))
            return false;
        webcam_generation_ = generation;
        ++webcam_upload_count_;
    }
    return DrawTexture(webcam_tex_.srv.get(), rect, mirror, chroma, true, opacity, err);
}

bool GpuCompositor::DrawWebcam(const WebcamFrameSnapshot& frame, const WebcamPixelRect& rect, bool mirror,
                               const ChromaKeyParams& chroma, std::string& err, float opacity) {
    if (!frame.HasCompletePayload())
        return true;
    if (frame.format == WebcamPixelFormat::Bgra) {
        return DrawWebcam(frame.pixels.data(), frame.width, frame.height, rect, mirror, chroma, err, opacity,
                          frame.generation);
    }
    const bool current = webcam_generation_ == frame.generation && webcam_tex_.rtv &&
                         webcam_tex_.width == static_cast<UINT>(frame.width) &&
                         webcam_tex_.height == static_cast<UINT>(frame.height);
    if (!current) {
        if (!ConvertWebcamYuv(frame, err))
            return false;
        webcam_generation_ = frame.generation;
        ++webcam_upload_count_;
    }
    return DrawTexture(webcam_tex_.srv.get(), rect, mirror, chroma, true, opacity, err);
}

bool GpuCompositor::ConvertWebcamYuv(const WebcamFrameSnapshot& frame, std::string& err) {
    const bool nv12 = frame.format == WebcamPixelFormat::Nv12;
    winrt::com_ptr<ID3D11PixelShader>& shader = nv12 ? nv12_shader_ : yuy2_shader_;
    if (!shader && !CompileWebcamShader(device_, nv12 ? kWebcamNv12PixelShaderSrc : kWebcamYuy2PixelShaderSrc,
                                        nv12 ? "D3DCompile(webcam nv12)" : "D3DCompile(webcam yuy2)", shader, err)) {
        return false;
    }
    if (!yuv_constants_) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(GpuYuvConstants);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const HRESULT hr = device_->CreateBuffer(&desc, nullptr, yuv_constants_.put());
        if (FAILED(hr)) {
            SetHResultError(err, "CreateBuffer(webcam yuv constants)", hr);
            return false;
        }
    }

    const UINT width = static_cast<UINT>(frame.width);
    const UINT height = static_cast<UINT>(frame.height);
    const uint8_t* bytes = frame.pixels.data();
    if (nv12) {
        if (!EnsureTexture(webcam_luma_, width, height, DXGI_FORMAT_R8_UINT, false, err) ||
            !EnsureTexture(webcam_chroma_, width / 2, height / 2, DXGI_FORMAT_R8G8_UINT, false, err)) {
            return false;
        }
        context_->UpdateSubresource(webcam_luma_.texture.get(), 0, nullptr, bytes, width, 0);
        context_->UpdateSubresource(webcam_chroma_.texture.get(), 0, nullptr,
                                    bytes + static_cast<size_t>(width) * height, width, 0);
    } else {
        if (!EnsureTexture(webcam_luma_, width / 2, height, DXGI_FORMAT_R8G8B8A8_UINT, false, err))
            return false;
        context_->UpdateSubresource(webcam_luma_.texture.get(), 0, nullptr, bytes, width * 2, 0);
    }
    if (!EnsureTexture(webcam_tex_, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, true, err))
        return false;

    const GpuYuvConstants constants = GpuYuvConstantsFor(frame.matrix, frame.range, false);
    context_->UpdateSubresource(yuv_constants_.get(), 0, nullptr, &constants, 0, 0);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    ID3D11RenderTargetView* rtv = webcam_tex_.rtv.get();
    ID3D11Buffer* buffer = yuv_constants_.get();
    ID3D11ShaderResourceView* planes[2] = {webcam_luma_.srv.get(), nv12 ? webcam_chroma_.srv.get() : nullptr};
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context_->RSSetViewports(1, &viewport);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_shader_.get(), nullptr, 0);
    context_->PSSetShader(shader.get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, &buffer);
    context_->PSSetShaderResources(0, 2, planes);
    context_->Draw(3, 0);

    // webcam_tex_ is sampled by the overlay pass next; leaving it bound as the
    // render target would make D3D11 null that shader-resource slot.
    ID3D11ShaderResourceView* null_planes[2] = {nullptr, nullptr};
    ID3D11RenderTargetView* null_rtv = nullptr;
    context_->PSSetShaderResources(0, 2, null_planes);
    context_->OMSetRenderTargets(1, &null_rtv, nullptr);
    return true;
}

bool GpuCompositor::EnsureTexture(TextureResource& resource, UINT width, UINT height, DXGI_FORMAT format,
                                  bool render_target, std::string& err) {
    if (resource.texture && resource.width == width && resource.height == height && resource.format == format &&
        static_cast<bool>(resource.rtv) == render_target) {
        return true;
    }
    resource = TextureResource{};

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (render_target ? D3D11_BIND_RENDER_TARGET : 0u);

    HRESULT hr = exosnap::engine::CreateTrackedTexture2D(device_, &desc, nullptr, resource.texture.put(),
                                                         exosnap::engine::GpuSurfaceOwner::WebcamCursor);
    if (FAILED(hr)) {
        SetHResultError(err, "CreateTexture2D(upload)", hr);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    hr = device_->CreateShaderResourceView(resource.texture.get(), &srv_desc, resource.srv.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateShaderResourceView(upload)", hr);
        resource = TextureResource{};
        return false;
    }
    if (render_target) {
        hr = device_->CreateRenderTargetView(resource.texture.get(), nullptr, resource.rtv.put());
        if (FAILED(hr)) {
            SetHResultError(err, "CreateRenderTargetView(upload)", hr);
            resource = TextureResource{};
            return false;
        }
    }
    resource.width = width;
    resource.height = height;
    resource.format = format;
    return true;
}

bool GpuCompositor::DrawCursor(const uint8_t* bgra, int width, int height, const WebcamPixelRect& rect,
                               std::string& err) {
    if (!UploadTexture(cursor_tex_, bgra, width, height, static_cast<UINT>(width * 4), err)) {
        return false;
    }

    ChromaKeyParams chroma;
    return DrawTexture(cursor_tex_.srv.get(), rect, false, chroma, false, 1.0f, err);
}

bool GpuCompositor::DrawCursorInvert(const uint8_t* bgra, int width, int height, const WebcamPixelRect& rect,
                                     std::string& err) {
    if (!UploadTexture(cursor_invert_tex_, bgra, width, height, static_cast<UINT>(width * 4), err)) {
        return false;
    }

    ChromaKeyParams chroma;
    if (MaskComposition() == MaskCursorComposition::HdrVisibleFallback) {
        // Not the Win32 operation, and deliberately so: see MaskCursorComposition.
        // The ordinary alpha blend draws the marked pixels and leaves the rest,
        // because the plane is zero (alpha included) everywhere else.
        return DrawTexture(cursor_invert_tex_.srv.get(), rect, false, chroma, false, 1.0f, err);
    }
    return DrawTexture(cursor_invert_tex_.srv.get(), rect, false, chroma, false, 1.0f, err, invert_blend_state_.get());
}

bool GpuCompositor::DrawSpriteLayer(size_t layer_index, const SpriteLayerSnapshot& layer,
                                    const WebcamPixelRect& content, std::string& err) {
    const SpriteAtlas* atlas = layer.atlas.get();
    if (atlas == nullptr || !atlas->HasCompletePayload() || layer.quads.empty())
        return true;
    if (sprite_atlases_.size() <= layer_index)
        sprite_atlases_.resize(layer_index + 1);
    SpriteAtlasTexture& cached = sprite_atlases_[layer_index];
    if (cached.generation != atlas->generation || !cached.texture.srv ||
        cached.texture.width != static_cast<UINT>(atlas->width) ||
        cached.texture.height != static_cast<UINT>(atlas->height)) {
        if (!UploadTexture(cached.texture, atlas->pixels.data(), atlas->width, atlas->height,
                           static_cast<UINT>(atlas->width) * 4u, err)) {
            cached.generation.reset();
            return false;
        }
        cached.generation = atlas->generation;
        ++sprite_atlas_upload_count_;
    }

    const ChromaKeyParams no_key;
    for (const SpriteQuad& quad : layer.quads) {
        const SpriteDraw draw =
            MapSpriteQuad(quad, atlas->width, atlas->height, content.x, content.y, content.w, content.h);
        if (!draw.IsValid())
            continue;
        const float uv_rect[4] = {draw.u0, draw.v0, draw.u1, draw.v1};
        if (!DrawTexture(cached.texture.srv.get(), draw.destination, false, no_key, false, draw.opacity, err, nullptr,
                         uv_rect)) {
            return false;
        }
        ++sprite_quad_draw_count_;
    }
    return true;
}

bool GpuCompositor::UploadTexture(TextureResource& resource, const uint8_t* bgra, int width, int height, UINT row_pitch,
                                  std::string& err) {
    if (device_ == nullptr || context_ == nullptr || bgra == nullptr || width <= 0 || height <= 0) {
        err = "GpuCompositor::UploadTexture invalid arguments";
        return false;
    }

    if (!EnsureTexture(resource, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_B8G8R8A8_UNORM, false,
                       err)) {
        return false;
    }
    context_->UpdateSubresource(resource.texture.get(), 0, nullptr, bgra, row_pitch, 0);
    return true;
}

bool GpuCompositor::DrawTexture(ID3D11ShaderResourceView* srv, const WebcamPixelRect& rect, bool mirror,
                                const ChromaKeyParams& chroma, bool force_opaque, float opacity, std::string& err,
                                ID3D11BlendState* blend, const float* uv_rect) {
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
    OverlayPixelConstants pc = MakeOverlayPixelConstants(chroma, mirror, force_opaque, opacity, hdr_linear_,
                                                         overlay_ref_white_nits_ / kScrgbReferenceWhiteNits);
    if (uv_rect != nullptr)
        std::memcpy(pc.uv_rect, uv_rect, sizeof(pc.uv_rect));
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
    context_->OMSetBlendState(blend == nullptr ? blend_state_.get() : blend, blend_factor, 0xffffffff);
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

    // Unbind BOTH, not just the input. composite_tex_ is what Result() hands the
    // caller, so leaving composite_rtv_ bound makes the next pass that samples
    // the composite hit a read/write hazard: D3D11 resolves it by nulling the
    // shader-resource slot, and a later OMSetRenderTargets does not restore it,
    // so that pass silently samples nothing. Same contract as HdrToneMapper.
    ID3D11ShaderResourceView* null_srv = nullptr;
    ID3D11RenderTargetView* null_rtv = nullptr;
    context_->PSSetShaderResources(0, 1, &null_srv);
    context_->OMSetRenderTargets(1, &null_rtv, nullptr);
    // The blend state too. This is the only pass in the engine that enables
    // alpha blending, and the context keeps it for every later draw on the
    // shared immediate context. A later pass whose shader does not write alpha
    // -- the P010 luma/chroma planes return float/float2 -- then blends against
    // an undefined alpha, and the result is driver-dependent.
    context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    return true;
}

} // namespace exosnap::engine
