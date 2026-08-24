#include "gpu_hdr_pq.h"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>

namespace exosnap::engine {
namespace {

const char* kVertexShaderSrc = R"(
struct VS_OUTPUT {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

VS_OUTPUT main(uint id : SV_VertexID) {
    VS_OUTPUT output;
    output.texcoord = float2((id << 1) & 2, id & 2);
    output.position = float4(output.texcoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}
)";

// Shared HLSL colour math. A verbatim mirror of the CPU reference in hdr_pq.h
// (PqOetf, ScrgbToPqNormalized, Bt709ToBt2020, PqRgbToYcbcr, quantiser). The CPU
// version is the unit-tested source of truth — keep the two in sync. Portable
// ALU only (no driver intrinsics) so the result is identical on every GPU.
//
// Output packing: P010 stores 10-bit samples left-justified in 16-bit words, so
// each code is written as code*64/65535 into the R16(G16)_UNORM plane view.
const char* kCommonHlsl = R"(
Texture2D<float4> srcTex : register(t0);
SamplerState srcSamp : register(s0);

cbuffer PqConstants : register(b0) {
    float4 cropOriginSize; // xy = crop origin, zw = crop size (normalised source)
    float4 flags;          // x = inputIsPq (0 = scRGB FP16, 1 = already PQ R'G'B')
};

static const float kM1 = 2610.0f / 16384.0f;
static const float kM2 = 2523.0f / 4096.0f * 128.0f;
static const float kC1 = 3424.0f / 4096.0f;
static const float kC2 = 2413.0f / 4096.0f * 32.0f;
static const float kC3 = 2392.0f / 4096.0f * 32.0f;
static const float kRefWhiteNits = 80.0f;
static const float kPqPeakNits = 10000.0f;
static const float kKr = 0.2627f;
static const float kKb = 0.0593f;
static const float kKg = 1.0f - kKr - kKb;

float PqOetf(float l) {
    l = saturate(l);
    float lm1 = pow(l, kM1);
    return pow((kC1 + kC2 * lm1) / (1.0f + kC3 * lm1), kM2);
}

float ScrgbToPqNorm(float v) {
    float nits = max(v, 0.0f) * kRefWhiteNits;
    return min(nits / kPqPeakNits, 1.0f);
}

float3 Bt709ToBt2020(float3 c) {
    return float3(
        0.6274038959f * c.r + 0.3292830384f * c.g + 0.0433130657f * c.b,
        0.0690972894f * c.r + 0.9195403951f * c.g + 0.0113623156f * c.b,
        0.0163914389f * c.r + 0.0880133079f * c.g + 0.8955952532f * c.b);
}

// HDR capture RGB -> non-linear PQ/BT.2020 R'G'B' in [0, 1].
float3 EncodedRgb(float3 rgb) {
    if (flags.x > 0.5f) {
        return saturate(rgb); // already PQ-encoded (HDR10 R10G10B10A2 desktop)
    }
    float3 lin = float3(ScrgbToPqNorm(rgb.r), ScrgbToPqNorm(rgb.g), ScrgbToPqNorm(rgb.b));
    lin = saturate(Bt709ToBt2020(lin));
    return float3(PqOetf(lin.r), PqOetf(lin.g), PqOetf(lin.b));
}

float3 SampleEncoded(float2 texcoord) {
    float2 uv = cropOriginSize.xy + texcoord * cropOriginSize.zw;
    return EncodedRgb(srcTex.Sample(srcSamp, uv).rgb);
}

// 10-bit limited-range codes -> P010 MSB-aligned UNORM value.
float PackLuma(float yprime) {
    float code = floor(saturate(yprime) * 876.0f + 64.0f + 0.5f);
    return code * 64.0f / 65535.0f;
}
float PackChroma(float c) {
    float code = clamp(floor(c * 896.0f + 512.0f + 0.5f), 0.0f, 1023.0f);
    return code * 64.0f / 65535.0f;
}
)";

const char* kLumaShaderSrc = R"(
float main(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    float3 rp = SampleEncoded(texcoord);
    float y = kKr * rp.r + kKg * rp.g + kKb * rp.b;
    return PackLuma(y);
}
)";

const char* kChromaShaderSrc = R"(
float2 main(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    float3 rp = SampleEncoded(texcoord);
    float y = kKr * rp.r + kKg * rp.g + kKb * rp.b;
    float cb = (rp.b - y) / (2.0f * (1.0f - kKb));
    float cr = (rp.r - y) / (2.0f * (1.0f - kKr));
    return float2(PackChroma(cb), PackChroma(cr));
}
)";

struct PqConstants {
    float crop_origin_size[4];
    float flags[4];
};

// P010 narrow-range plane clear colours (MSB-aligned): luma black = 64, chroma
// neutral = 512.
constexpr float kLumaBlack = 64.0f * 64.0f / 65535.0f;
constexpr float kChromaNeutral = 512.0f * 64.0f / 65535.0f;

void SetHResultError(std::string& err, const char* what, HRESULT hr) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s failed 0x%08lX", what, static_cast<unsigned long>(hr));
    err = buf;
}

bool CompilePs(ID3D11Device* device, const std::string& src, const char* name, ID3D11PixelShader** out,
               std::string& err) {
    winrt::com_ptr<ID3DBlob> blob;
    winrt::com_ptr<ID3DBlob> error_blob;
    HRESULT hr = D3DCompile(src.c_str(), src.size(), name, nullptr, nullptr, "main", "ps_5_0",
                            D3DCOMPILE_ENABLE_STRICTNESS, 0, blob.put(), error_blob.put());
    if (FAILED(hr)) {
        SetHResultError(err, "D3DCompile(hdr_pq pixel shader)", hr);
        return false;
    }
    hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out);
    if (FAILED(hr)) {
        SetHResultError(err, "CreatePixelShader(hdr_pq)", hr);
        return false;
    }
    return true;
}

} // namespace

bool HdrPqConverter::Init(ID3D11Device* device, ID3D11DeviceContext* context, const Geometry& geom, bool input_is_pq,
                          DXGI_FORMAT src_format, std::string& err) {
    if (device == nullptr || context == nullptr || geom.encode_width == 0 || geom.encode_height == 0 ||
        geom.src_width == 0 || geom.src_height == 0 || geom.content_w == 0 || geom.content_h == 0) {
        err = "HdrPqConverter::Init invalid arguments";
        return false;
    }
    device_ = device;
    context_ = context;
    geom_ = geom;
    src_format_ = src_format;

    winrt::com_ptr<ID3DBlob> vs_blob;
    winrt::com_ptr<ID3DBlob> error_blob;
    HRESULT hr = D3DCompile(kVertexShaderSrc, std::strlen(kVertexShaderSrc), "hdr_pq_vs", nullptr, nullptr, "main",
                            "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, vs_blob.put(), error_blob.put());
    if (FAILED(hr)) {
        SetHResultError(err, "D3DCompile(hdr_pq vertex shader)", hr);
        return false;
    }
    hr = device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr,
                                     vertex_shader_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateVertexShader(hdr_pq)", hr);
        return false;
    }

    const std::string common = kCommonHlsl;
    if (!CompilePs(device_, common + kLumaShaderSrc, "hdr_pq_luma_ps", luma_shader_.put(), err)) {
        return false;
    }
    if (!CompilePs(device_, common + kChromaShaderSrc, "hdr_pq_chroma_ps", chroma_shader_.put(), err)) {
        return false;
    }

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; // bilinear for contain-fit scaling
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&sampler_desc, sampler_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateSamplerState(hdr_pq)", hr);
        return false;
    }

    PqConstants pc{};
    pc.crop_origin_size[0] = static_cast<float>(geom.src_crop_x) / static_cast<float>(geom.src_width);
    pc.crop_origin_size[1] = static_cast<float>(geom.src_crop_y) / static_cast<float>(geom.src_height);
    pc.crop_origin_size[2] = static_cast<float>(geom.src_crop_w) / static_cast<float>(geom.src_width);
    pc.crop_origin_size[3] = static_cast<float>(geom.src_crop_h) / static_cast<float>(geom.src_height);
    pc.flags[0] = input_is_pq ? 1.0f : 0.0f;

    D3D11_BUFFER_DESC const_desc{};
    const_desc.ByteWidth = sizeof(PqConstants);
    const_desc.Usage = D3D11_USAGE_DEFAULT;
    const_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA const_data{};
    const_data.pSysMem = &pc;
    hr = device_->CreateBuffer(&const_desc, &const_data, constants_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateBuffer(hdr_pq constants)", hr);
        return false;
    }
    return true;
}

ID3D11ShaderResourceView* HdrPqConverter::SrvFor(ID3D11Texture2D* tex, std::string& err) {
    auto it = srv_cache_.find(tex);
    if (it != srv_cache_.end()) {
        return it->second.get();
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = src_format_;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    const HRESULT hr = device_->CreateShaderResourceView(tex, &srv_desc, srv.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateShaderResourceView(hdr_pq src)", hr);
        return nullptr;
    }
    ID3D11ShaderResourceView* raw = srv.get();
    srv_cache_.emplace(tex, std::move(srv));
    return raw;
}

ID3D11RenderTargetView*
HdrPqConverter::PlaneRtvFor(ID3D11Texture2D* tex, DXGI_FORMAT plane_format,
                            std::unordered_map<ID3D11Texture2D*, winrt::com_ptr<ID3D11RenderTargetView>>& cache,
                            std::string& err) {
    auto it = cache.find(tex);
    if (it != cache.end()) {
        return it->second.get();
    }
    // A P010 plane is addressed by a UNORM view whose channel count selects the
    // plane: R16_UNORM = luma, R16G16_UNORM = chroma.
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
    rtv_desc.Format = plane_format;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtv_desc.Texture2D.MipSlice = 0;
    winrt::com_ptr<ID3D11RenderTargetView> rtv;
    const HRESULT hr = device_->CreateRenderTargetView(tex, &rtv_desc, rtv.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateRenderTargetView(hdr_pq P010 plane)", hr);
        return nullptr;
    }
    ID3D11RenderTargetView* raw = rtv.get();
    cache.emplace(tex, std::move(rtv));
    return raw;
}

bool HdrPqConverter::Convert(ID3D11Texture2D* src, ID3D11Texture2D* dst, std::string& err) {
    if (context_ == nullptr || src == nullptr || dst == nullptr) {
        err = "HdrPqConverter::Convert called before Init or with null texture";
        return false;
    }
    ID3D11ShaderResourceView* srv = SrvFor(src, err);
    if (srv == nullptr) {
        return false;
    }
    ID3D11RenderTargetView* luma_rtv = PlaneRtvFor(dst, DXGI_FORMAT_R16_UNORM, luma_rtv_cache_, err);
    if (luma_rtv == nullptr) {
        return false;
    }
    ID3D11RenderTargetView* chroma_rtv = PlaneRtvFor(dst, DXGI_FORMAT_R16G16_UNORM, chroma_rtv_cache_, err);
    if (chroma_rtv == nullptr) {
        return false;
    }

    ID3D11SamplerState* sampler = sampler_.get();
    ID3D11Buffer* constants = constants_.get();
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_shader_.get(), nullptr, 0);
    context_->PSSetSamplers(0, 1, &sampler);
    context_->PSSetConstantBuffers(0, 1, &constants);
    context_->PSSetShaderResources(0, 1, &srv);

    // --- Luma plane (full encode resolution) ---
    const float luma_clear[4] = {kLumaBlack, kLumaBlack, kLumaBlack, kLumaBlack};
    context_->ClearRenderTargetView(luma_rtv, luma_clear); // letterbox bars
    context_->OMSetRenderTargets(1, &luma_rtv, nullptr);
    D3D11_VIEWPORT luma_vp{};
    luma_vp.TopLeftX = static_cast<float>(geom_.content_x);
    luma_vp.TopLeftY = static_cast<float>(geom_.content_y);
    luma_vp.Width = static_cast<float>(geom_.content_w);
    luma_vp.Height = static_cast<float>(geom_.content_h);
    luma_vp.MinDepth = 0.0f;
    luma_vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &luma_vp);
    context_->PSSetShader(luma_shader_.get(), nullptr, 0);
    context_->Draw(3, 0);

    // --- Chroma plane (half resolution, 4:2:0) ---
    const float chroma_clear[4] = {kChromaNeutral, kChromaNeutral, kChromaNeutral, kChromaNeutral};
    context_->ClearRenderTargetView(chroma_rtv, chroma_clear);
    context_->OMSetRenderTargets(1, &chroma_rtv, nullptr);
    D3D11_VIEWPORT chroma_vp{};
    chroma_vp.TopLeftX = static_cast<float>(geom_.content_x / 2);
    chroma_vp.TopLeftY = static_cast<float>(geom_.content_y / 2);
    chroma_vp.Width = static_cast<float>(geom_.content_w / 2);
    chroma_vp.Height = static_cast<float>(geom_.content_h / 2);
    chroma_vp.MinDepth = 0.0f;
    chroma_vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &chroma_vp);
    context_->PSSetShader(chroma_shader_.get(), nullptr, 0);
    context_->Draw(3, 0);

    // Unbind so dst can be encoded / copied without a read/write hazard.
    ID3D11ShaderResourceView* null_srv = nullptr;
    ID3D11RenderTargetView* null_rtv = nullptr;
    context_->PSSetShaderResources(0, 1, &null_srv);
    context_->OMSetRenderTargets(1, &null_rtv, nullptr);
    return true;
}

} // namespace exosnap::engine
