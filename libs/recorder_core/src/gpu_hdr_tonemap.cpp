#include <recorder_core/gpu_hdr_tonemap.h>

#include "hdr_tonemap.h"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>

namespace recorder_core {
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

// Per-channel scRGB(HDR) -> SDR BT.709. This HLSL is a verbatim copy of the CPU
// reference in hdr_tonemap.h (HdrToneMapChannel + Bt709Oetf); the kKnee constant
// mirrors kHdrToneMapKnee there. Keep the two in sync — the CPU version is the
// unit-tested source of truth. Uses only portable ALU (no driver intrinsics) so
// the result is identical on every GPU.
const char* kPixelShaderSrc = R"(
Texture2D<float4> srcTex : register(t0);
SamplerState srcSamp : register(s0);

cbuffer ToneMapConstants : register(b0) {
    float4 params; // x = peakScale (reference-white multiples that map to 1.0)
                   // y = 1.0 when the source is an SDR scRGB (Advanced Color)
                   //     desktop: clamp + sRGB OETF, no roll-off
};

static const float kKnee = 0.80f;

float ToneMapChannel(float x, float peak) {
    x = max(x, 0.0f);
    if (x <= kKnee || peak <= kKnee) {
        return min(x, 1.0f);
    }
    const float head = 1.0f - kKnee;
    const float maxExcess = peak - kKnee;
    const float e = x - kKnee;
    const float s = e * (1.0f + e / (maxExcess * maxExcess)) / (1.0f + e);
    return min(kKnee + head * s, 1.0f);
}

float Bt709Oetf(float l) {
    l = saturate(l);
    return l < 0.018f ? 4.5f * l : 1.099f * pow(l, 0.45f) - 0.099f;
}

float SrgbOetf(float l) {
    l = saturate(l);
    return l <= 0.0031308f ? 12.92f * l : 1.055f * pow(l, 1.0f / 2.4f) - 0.055f;
}

float4 main(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    float4 c = srcTex.Sample(srcSamp, texcoord);
    if (params.y > 0.5f) {
        // SDR scRGB desktop: content already ends at reference white (1.0).
        float3 s = float3(SrgbOetf(c.r), SrgbOetf(c.g), SrgbOetf(c.b));
        return float4(s, 1.0f);
    }
    const float peak = params.x;
    float3 lin = float3(ToneMapChannel(c.r, peak), ToneMapChannel(c.g, peak), ToneMapChannel(c.b, peak));
    float3 sig = float3(Bt709Oetf(lin.r), Bt709Oetf(lin.g), Bt709Oetf(lin.b));
    return float4(sig, 1.0f);
}
)";

struct ToneMapConstants {
    float params[4]; // x = peak scale, y = sdr scRGB source flag, z/w reserved
};

void SetHResultError(std::string& err, const char* what, HRESULT hr) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s failed 0x%08lX", what, static_cast<unsigned long>(hr));
    err = buf;
}

} // namespace

bool HdrToneMapper::Init(ID3D11Device* device, ID3D11DeviceContext* context, UINT width, UINT height, float peak_scale,
                         bool sdr_scrgb_source, std::string& err) {
    if (device == nullptr || context == nullptr || width == 0 || height == 0) {
        err = "HdrToneMapper::Init invalid arguments";
        return false;
    }

    device_ = device;
    context_ = context;
    width_ = width;
    height_ = height;

    winrt::com_ptr<ID3DBlob> vs_blob;
    winrt::com_ptr<ID3DBlob> ps_blob;
    winrt::com_ptr<ID3DBlob> error_blob;

    HRESULT hr = D3DCompile(kVertexShaderSrc, std::strlen(kVertexShaderSrc), "hdr_tonemap_vs", nullptr, nullptr, "main",
                            "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, vs_blob.put(), error_blob.put());
    if (FAILED(hr)) {
        SetHResultError(err, "D3DCompile(tone-map vertex shader)", hr);
        return false;
    }

    error_blob = nullptr;
    hr = D3DCompile(kPixelShaderSrc, std::strlen(kPixelShaderSrc), "hdr_tonemap_ps", nullptr, nullptr, "main", "ps_5_0",
                    D3DCOMPILE_ENABLE_STRICTNESS, 0, ps_blob.put(), error_blob.put());
    if (FAILED(hr)) {
        SetHResultError(err, "D3DCompile(tone-map pixel shader)", hr);
        return false;
    }

    hr = device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr,
                                     vertex_shader_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateVertexShader(tone-map)", hr);
        return false;
    }
    hr =
        device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, pixel_shader_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreatePixelShader(tone-map)", hr);
        return false;
    }

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; // 1:1 tone-map; no filtering
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&sampler_desc, sampler_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateSamplerState(tone-map)", hr);
        return false;
    }

    ToneMapConstants pc{};
    pc.params[0] = peak_scale;
    pc.params[1] = sdr_scrgb_source ? 1.0f : 0.0f;

    D3D11_BUFFER_DESC const_desc{};
    const_desc.ByteWidth = sizeof(ToneMapConstants);
    const_desc.Usage = D3D11_USAGE_DEFAULT;
    const_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA const_data{};
    const_data.pSysMem = &pc;
    hr = device_->CreateBuffer(&const_desc, &const_data, constants_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateBuffer(tone-map constants)", hr);
        return false;
    }

    return true;
}

ID3D11ShaderResourceView* HdrToneMapper::SrvFor(ID3D11Texture2D* tex, std::string& err) {
    auto it = srv_cache_.find(tex);
    if (it != srv_cache_.end()) {
        return it->second.get();
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    const HRESULT hr = device_->CreateShaderResourceView(tex, &srv_desc, srv.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateShaderResourceView(tone-map src)", hr);
        return nullptr;
    }
    ID3D11ShaderResourceView* raw = srv.get();
    srv_cache_.emplace(tex, std::move(srv));
    return raw;
}

ID3D11RenderTargetView* HdrToneMapper::RtvFor(ID3D11Texture2D* tex, std::string& err) {
    auto it = rtv_cache_.find(tex);
    if (it != rtv_cache_.end()) {
        return it->second.get();
    }
    winrt::com_ptr<ID3D11RenderTargetView> rtv;
    const HRESULT hr = device_->CreateRenderTargetView(tex, nullptr, rtv.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateRenderTargetView(tone-map dst)", hr);
        return nullptr;
    }
    ID3D11RenderTargetView* raw = rtv.get();
    rtv_cache_.emplace(tex, std::move(rtv));
    return raw;
}

bool HdrToneMapper::Convert(ID3D11Texture2D* src, ID3D11Texture2D* dst, std::string& err) {
    if (context_ == nullptr || src == nullptr || dst == nullptr) {
        err = "HdrToneMapper::Convert called before Init or with null texture";
        return false;
    }

    ID3D11ShaderResourceView* srv = SrvFor(src, err);
    if (srv == nullptr) {
        return false;
    }
    ID3D11RenderTargetView* rtv = RtvFor(dst, err);
    if (rtv == nullptr) {
        return false;
    }

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    context_->OMSetRenderTargets(1, &rtv, nullptr);
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

    // Unbind so dst can immediately serve as a VideoProcessor input and src can
    // be a CopyResource destination again without a read/write hazard.
    ID3D11ShaderResourceView* null_srv = nullptr;
    ID3D11RenderTargetView* null_rtv = nullptr;
    context_->PSSetShaderResources(0, 1, &null_srv);
    context_->OMSetRenderTargets(1, &null_rtv, nullptr);
    return true;
}

} // namespace recorder_core
