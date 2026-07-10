#include "gpu_rgb_to_ayuv.h"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>

namespace recorder_core {
namespace {

// RGB (full-range, 0..1) -> BT.709 Y'CbCr, packed into AYUV.
//
// The quantization range is selected by cfg.x (0 = limited/studio 16-235,
// 1 = full 0-255), matching the range the session's ColorMetadata signals and
// the range the 4:2:0 VideoProcessor path is configured with, so 4:4:4 and
// 4:2:0 recordings of the same desktop carry the same luma levels.
//
// Rounding uses floor(x + 0.5) (round-half-up) in BOTH this shader and the CPU
// reference in the golden test, so the two agree bit-for-bit regardless of the
// intrinsic rounding mode. Output goes through a DXGI_FORMAT_R8G8B8A8_UINT UAV
// whose channels map onto the AYUV byte sequence [V, U, Y, A]; hence the store
// order uint4(V, U, Y, 255). Only portable ALU is used so WARP and real GPUs
// produce identical bytes.
const char* kComputeShaderSrc = R"(
Texture2D<float4> srcTex : register(t0);
RWTexture2D<uint4> dstTex : register(u0);

cbuffer AyuvConstants : register(b0) {
    uint4 cfg; // x = fullRange (0 = studio/limited, 1 = full)
};

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint w = 0u;
    uint h = 0u;
    dstTex.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    // BGRA8 SRV is swizzled to RGBA, so .rgb is full-range R,G,B in [0,1].
    float3 c = srcTex.Load(int3(tid.xy, 0)).rgb;
    float R = c.r;
    float G = c.g;
    float B = c.b;

    float Y = 0.2126f * R + 0.7152f * G + 0.0722f * B;
    float Cb = (B - Y) / 1.8556f;
    float Cr = (R - Y) / 1.5748f;

    float yf, uf, vf;
    if (cfg.x != 0u) {
        // Full range (0-255).
        yf = Y * 255.0f;
        uf = Cb * 255.0f + 128.0f;
        vf = Cr * 255.0f + 128.0f;
    } else {
        // Limited/studio range (Y 16-235, chroma 16-240 centred on 128).
        yf = 16.0f + Y * 219.0f;
        uf = 128.0f + Cb * 224.0f;
        vf = 128.0f + Cr * 224.0f;
    }

    uint Yi = (uint)clamp(floor(yf + 0.5f), 0.0f, 255.0f);
    uint Ui = (uint)clamp(floor(uf + 0.5f), 0.0f, 255.0f);
    uint Vi = (uint)clamp(floor(vf + 0.5f), 0.0f, 255.0f);

    // AYUV memory bytes are [V, U, Y, A]; the R8G8B8A8_UINT UAV maps x->byte0,
    // y->byte1, z->byte2, w->byte3.
    dstTex[tid.xy] = uint4(Vi, Ui, Yi, 255u);
}
)";

struct AyuvConstants {
    uint32_t cfg[4]; // x = fullRange, y/z/w reserved
};

void SetHResultError(std::string& err, const char* what, HRESULT hr) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s failed 0x%08lX", what, static_cast<unsigned long>(hr));
    err = buf;
}

} // namespace

bool RgbToAyuvConverter::Init(ID3D11Device* device, ID3D11DeviceContext* context, UINT width, UINT height,
                              bool full_range, std::string& err) {
    if (device == nullptr || context == nullptr || width == 0 || height == 0) {
        err = "RgbToAyuvConverter::Init invalid arguments";
        return false;
    }

    device_ = device;
    context_ = context;
    width_ = width;
    height_ = height;

    winrt::com_ptr<ID3DBlob> cs_blob;
    winrt::com_ptr<ID3DBlob> error_blob;
    HRESULT hr = D3DCompile(kComputeShaderSrc, std::strlen(kComputeShaderSrc), "rgb_to_ayuv_cs", nullptr, nullptr,
                            "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, cs_blob.put(), error_blob.put());
    if (FAILED(hr)) {
        SetHResultError(err, "D3DCompile(rgb->ayuv compute shader)", hr);
        return false;
    }

    hr = device_->CreateComputeShader(cs_blob->GetBufferPointer(), cs_blob->GetBufferSize(), nullptr, shader_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateComputeShader(rgb->ayuv)", hr);
        return false;
    }

    AyuvConstants cc{};
    cc.cfg[0] = full_range ? 1u : 0u;

    D3D11_BUFFER_DESC const_desc{};
    const_desc.ByteWidth = sizeof(AyuvConstants);
    const_desc.Usage = D3D11_USAGE_DEFAULT;
    const_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA const_data{};
    const_data.pSysMem = &cc;
    hr = device_->CreateBuffer(&const_desc, &const_data, constants_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateBuffer(rgb->ayuv constants)", hr);
        return false;
    }

    return true;
}

ID3D11ShaderResourceView* RgbToAyuvConverter::SrvFor(ID3D11Texture2D* tex, std::string& err) {
    auto it = srv_cache_.find(tex);
    if (it != srv_cache_.end()) {
        return it->second.get();
    }
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    // View an 8-bit RGBA/BGRA source as normalized float4; UNORM decode gives the
    // exact 1/255 multiples the CPU reference expects.
    srv_desc.Format =
        (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    const HRESULT hr = device_->CreateShaderResourceView(tex, &srv_desc, srv.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateShaderResourceView(rgb->ayuv src)", hr);
        return nullptr;
    }
    ID3D11ShaderResourceView* raw = srv.get();
    srv_cache_.emplace(tex, std::move(srv));
    return raw;
}

ID3D11UnorderedAccessView* RgbToAyuvConverter::UavFor(ID3D11Texture2D* tex, std::string& err) {
    auto it = uav_cache_.find(tex);
    if (it != uav_cache_.end()) {
        return it->second.get();
    }
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    // R8G8B8A8_UINT is the documented cast-compatible UAV view format for a
    // DXGI_FORMAT_AYUV resource (and trivially valid for an R8G8B8A8_UINT one in
    // the WARP golden test). Exact integer writes — no UNORM quantization.
    uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uav_desc.Texture2D.MipSlice = 0;
    winrt::com_ptr<ID3D11UnorderedAccessView> uav;
    const HRESULT hr = device_->CreateUnorderedAccessView(tex, &uav_desc, uav.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateUnorderedAccessView(rgb->ayuv dst)", hr);
        return nullptr;
    }
    ID3D11UnorderedAccessView* raw = uav.get();
    uav_cache_.emplace(tex, std::move(uav));
    return raw;
}

bool RgbToAyuvConverter::Convert(ID3D11Texture2D* src, ID3D11Texture2D* dst, std::string& err) {
    if (context_ == nullptr || src == nullptr || dst == nullptr) {
        err = "RgbToAyuvConverter::Convert called before Init or with null texture";
        return false;
    }

    ID3D11ShaderResourceView* srv = SrvFor(src, err);
    if (srv == nullptr) {
        return false;
    }
    ID3D11UnorderedAccessView* uav = UavFor(dst, err);
    if (uav == nullptr) {
        return false;
    }

    context_->CSSetShader(shader_.get(), nullptr, 0);
    ID3D11Buffer* constants = constants_.get();
    context_->CSSetConstantBuffers(0, 1, &constants);
    context_->CSSetShaderResources(0, 1, &srv);
    UINT init_counts = 0;
    context_->CSSetUnorderedAccessViews(0, 1, &uav, &init_counts);

    const UINT groups_x = (width_ + 7u) / 8u;
    const UINT groups_y = (height_ + 7u) / 8u;
    context_->Dispatch(groups_x, groups_y, 1);

    // Unbind so dst can be handed to NVENC / copied and src reused without a hazard.
    ID3D11ShaderResourceView* null_srv = nullptr;
    ID3D11UnorderedAccessView* null_uav = nullptr;
    context_->CSSetShaderResources(0, 1, &null_srv);
    context_->CSSetUnorderedAccessViews(0, 1, &null_uav, &init_counts);
    context_->CSSetShader(nullptr, nullptr, 0);
    return true;
}

} // namespace recorder_core
