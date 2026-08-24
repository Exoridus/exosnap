// WARP-backed GPU tests for the scRGB(HDR) -> SDR BT.709 tone-map render pass
// (gpu_hdr_tonemap.*). The pure curve math is pinned in test_hdr_tonemap.cpp;
// these tests prove the shader renders that curve correctly into both the 8-bit
// BGRA8 intermediate and the 10-bit R10G10B10A2 intermediate, and that the
// 10-bit target preserves precision the 8-bit target destroys.

#include <gtest/gtest.h>

#include "hdr_tonemap.h"

#include <exosnap/engine/gpu_hdr_tonemap.h>

#include <d3d11.h>
#include <winrt/base.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

namespace {

using exosnap::engine::HdrToneMapper;

struct D3DTestDevice {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
};

D3DTestDevice CreateWarpDevice() {
    D3DTestDevice out;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected{};
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                         out.device.put(), &selected, out.context.put());
    EXPECT_TRUE(SUCCEEDED(hr));
    return out;
}

// --- half <-> float (IEEE 754 binary16) ------------------------------------

float HalfToFloat(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            exp = 127 - 15 + 1;
            while ((mant & 0x400u) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// Truncating float -> half (round toward zero). Good enough for the positive
// mid-range values used here; tests always derive expectations from the
// round-tripped value so the CPU reference sees exactly what the GPU samples.
uint16_t FloatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, sizeof(x));
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exp = static_cast<int32_t>((x >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0) {
        return static_cast<uint16_t>(sign);
    }
    if (exp >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
}

// The scRGB value the shader actually samples for a requested linear input.
float RoundTrip(float v) {
    return HalfToFloat(FloatToHalf(v));
}

// Build a 1-row scRGB FP16 (R16G16B16A16_FLOAT) shader-resource source; each
// entry paints one grey pixel (r=g=b=value, a=1).
winrt::com_ptr<ID3D11Texture2D> CreateFp16Source(ID3D11Device* device, const std::vector<float>& greys) {
    const int width = static_cast<int>(greys.size());
    std::vector<uint16_t> halfs(static_cast<size_t>(width) * 4);
    for (int i = 0; i < width; ++i) {
        const uint16_t h = FloatToHalf(greys[static_cast<size_t>(i)]);
        halfs[static_cast<size_t>(i) * 4 + 0] = h;
        halfs[static_cast<size_t>(i) * 4 + 1] = h;
        halfs[static_cast<size_t>(i) * 4 + 2] = h;
        halfs[static_cast<size_t>(i) * 4 + 3] = FloatToHalf(1.0f);
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = halfs.data();
    init.SysMemPitch = static_cast<UINT>(width * 4 * 2);
    winrt::com_ptr<ID3D11Texture2D> tex;
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, &init, tex.put())));
    return tex;
}

winrt::com_ptr<ID3D11Texture2D> CreateRenderTarget(ID3D11Device* device, DXGI_FORMAT format, int width) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    winrt::com_ptr<ID3D11Texture2D> tex;
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, tex.put())));
    return tex;
}

// 32-bit-per-texel readback (covers BGRA8 and R10G10B10A2; both are 4 bytes).
std::vector<uint32_t> Read32(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    winrt::com_ptr<ID3D11Texture2D> staging;
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, staging.put())));
    context->CopyResource(staging.get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    EXPECT_TRUE(SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
    std::vector<uint32_t> out(static_cast<size_t>(desc.Width) * desc.Height);
    std::memcpy(out.data(), mapped.pData, out.size() * 4); // single row
    context->Unmap(staging.get(), 0);
    return out;
}

// Tone-map a row of grey scRGB inputs into `format`; return raw 32-bit texels.
std::vector<uint32_t> ToneMapRow(D3DTestDevice& d3d, DXGI_FORMAT format, const std::vector<float>& greys,
                                 float peak_scale, bool sdr_scrgb_source = false) {
    const int width = static_cast<int>(greys.size());
    auto src = CreateFp16Source(d3d.device.get(), greys);
    auto dst = CreateRenderTarget(d3d.device.get(), format, width);
    HdrToneMapper mapper;
    std::string err;
    EXPECT_TRUE(mapper.Init(d3d.device.get(), d3d.context.get(), static_cast<UINT>(width), 1, peak_scale,
                            sdr_scrgb_source, err))
        << err;
    EXPECT_TRUE(mapper.Convert(src.get(), dst.get(), err)) << err;
    return Read32(d3d.device.get(), d3d.context.get(), dst.get());
}

// R channel of an R10G10B10A2 texel (bits 0-9).
uint32_t R10(uint32_t texel) {
    return texel & 0x3FFu;
}
// R channel of a BGRA8 texel (byte 2: B,G,R,A little-endian).
uint32_t R8(uint32_t texel) {
    return (texel >> 16) & 0xFFu;
}

constexpr float kPeak400 = 5.0f; // 400 cd/m^2 display peak.

// --- correctness: R10G10B10A2 output matches the CPU reference --------------

TEST(GpuHdrToneMapR10, MatchesCpuReferenceAcrossRange) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);

    const std::vector<float> greys = {0.0f, 0.25f, 0.5f, 0.8f, 1.0f, 2.5f, kPeak400, 8.0f};
    const auto texels = ToneMapRow(d3d, DXGI_FORMAT_R10G10B10A2_UNORM, greys, kPeak400);
    ASSERT_EQ(texels.size(), greys.size());

    for (size_t i = 0; i < greys.size(); ++i) {
        const float sig = exosnap::engine::ScrgbToSdr709Channel(RoundTrip(greys[i]), kPeak400);
        const auto expected = static_cast<uint32_t>(std::lround(sig * 1023.0f));
        EXPECT_NEAR(static_cast<int>(R10(texels[i])), static_cast<int>(expected), 2)
            << "grey=" << greys[i] << " sig=" << sig;
    }
}

// --- SDR scRGB (Advanced Color desktop): sRGB encode, no roll-off -----------

TEST(GpuHdrToneMapR10, SdrScrgbSourceEncodesWithSrgbAndKeepsWhite) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);

    // An SDR Advanced-Color desktop never exceeds reference white (measured: the
    // FP16 desktop peaks at exactly 1.0), so the pass must be a plain sRGB encode.
    const std::vector<float> greys = {0.0f, 0.0144f, 0.2159f, 0.5271f, 0.8f, 1.0f};
    const auto texels =
        ToneMapRow(d3d, DXGI_FORMAT_B8G8R8A8_UNORM, greys, /*peak_scale=*/12.5f, /*sdr_scrgb_source=*/true);
    ASSERT_EQ(texels.size(), greys.size());

    for (size_t i = 0; i < greys.size(); ++i) {
        const float sig = exosnap::engine::ScrgbSdrToSrgbChannel(RoundTrip(greys[i]));
        const auto expected = static_cast<uint32_t>(std::lround(sig * 255.0f));
        EXPECT_NEAR(static_cast<int>(R8(texels[i])), static_cast<int>(expected), 1) << "grey=" << greys[i];
    }
    // The regression this guards: white must reach 255, not the 233 the HDR
    // roll-off would produce, and the peak_scale passed above must be ignored.
    EXPECT_EQ(R8(texels.back()), 255u);
    // Mid-grey (sRGB code 128) round-trips exactly, as it must on every path.
    EXPECT_NEAR(static_cast<int>(R8(texels[2])), 128, 1);
}

// The 8-bit path stays byte-identical to the historic behaviour.
TEST(GpuHdrToneMapR10, Bgra8OutputMatchesCpuReference) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);

    const std::vector<float> greys = {0.0f, 0.5f, 1.0f, kPeak400};
    const auto texels = ToneMapRow(d3d, DXGI_FORMAT_B8G8R8A8_UNORM, greys, kPeak400);
    ASSERT_EQ(texels.size(), greys.size());

    for (size_t i = 0; i < greys.size(); ++i) {
        const float sig = exosnap::engine::ScrgbToSdr709Channel(RoundTrip(greys[i]), kPeak400);
        const auto expected = static_cast<uint32_t>(std::lround(sig * 255.0f));
        EXPECT_NEAR(static_cast<int>(R8(texels[i])), static_cast<int>(expected), 1) << "grey=" << greys[i];
    }
}

// --- the feature proof: 10-bit distinguishes what 8-bit collapses ----------

TEST(GpuHdrToneMapR10, TenBitPreservesPrecisionEightBitDestroys) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);

    // Two scRGB inputs whose tone-mapped SDR signals sit in the same 8-bit code
    // but are several 10-bit codes apart. Both are below the tone-map knee
    // (identity), so the signal is SrgbOetf of the input.
    const float x1 = RoundTrip(0.3642f);
    const float x2 = RoundTrip(0.3682f);

    const float sig1 = exosnap::engine::ScrgbToSdr709Channel(x1, kPeak400);
    const float sig2 = exosnap::engine::ScrgbToSdr709Channel(x2, kPeak400);

    // Precondition documenting the chosen constants: identical in 8-bit, distinct
    // in 10-bit. If this ever fails, the input constants need re-tuning — it is
    // not a regression in the pipeline under test.
    ASSERT_EQ(std::lround(sig1 * 255.0f), std::lround(sig2 * 255.0f)) << "sig1=" << sig1 << " sig2=" << sig2;
    ASSERT_NE(std::lround(sig1 * 1023.0f), std::lround(sig2 * 1023.0f)) << "sig1=" << sig1 << " sig2=" << sig2;

    const std::vector<float> greys = {x1, x2};

    // 8-bit intermediate: the two inputs collapse to the same code — precision lost.
    const auto bgra = ToneMapRow(d3d, DXGI_FORMAT_B8G8R8A8_UNORM, greys, kPeak400);
    ASSERT_EQ(bgra.size(), 2u);
    EXPECT_EQ(R8(bgra[0]), R8(bgra[1])) << "8-bit target must not distinguish the two inputs";

    // 10-bit intermediate: the two inputs stay distinct — precision preserved.
    const auto r10 = ToneMapRow(d3d, DXGI_FORMAT_R10G10B10A2_UNORM, greys, kPeak400);
    ASSERT_EQ(r10.size(), 2u);
    EXPECT_NE(R10(r10[0]), R10(r10[1])) << "10-bit target must distinguish the two inputs";
}

} // namespace
