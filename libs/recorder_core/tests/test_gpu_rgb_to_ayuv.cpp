// WARP-backed golden tests for the RGB -> packed AYUV (4:4:4, 8-bit) compute
// shader (gpu_rgb_to_ayuv.*). Synthetic full-range BGRA8 pixels go in; the test
// reads back the raw AYUV bytes and compares them to a CPU reference that runs
// the identical BT.709 conversion with the same floor(x+0.5) rounding, for both
// the limited/studio (encode default) and full quantization ranges.
//
// AYUV byte layout is [V, U, Y, A]; the converter writes through an
// R8G8B8A8_UINT UAV, so a read-back 32-bit texel is V | U<<8 | Y<<16 | A<<24.

#include <gtest/gtest.h>

#include "gpu_rgb_to_ayuv.h"

#include <d3d11.h>
#include <winrt/base.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using recorder_core::RgbToAyuvConverter;

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

struct Rgb8 {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

struct Ayuv8 {
    uint8_t y;
    uint8_t u;
    uint8_t v;
};

// CPU reference: BT.709 RGB(0..1) -> Y'CbCr, matching the shader exactly
// (same coefficients, same floor(x+0.5) rounding).
Ayuv8 RefRgbToAyuv(Rgb8 c, bool full_range) {
    const float R = static_cast<float>(c.r) / 255.0f;
    const float G = static_cast<float>(c.g) / 255.0f;
    const float B = static_cast<float>(c.b) / 255.0f;
    const float Y = 0.2126f * R + 0.7152f * G + 0.0722f * B;
    const float Cb = (B - Y) / 1.8556f;
    const float Cr = (R - Y) / 1.5748f;
    float yf, uf, vf;
    if (full_range) {
        yf = Y * 255.0f;
        uf = Cb * 255.0f + 128.0f;
        vf = Cr * 255.0f + 128.0f;
    } else {
        yf = 16.0f + Y * 219.0f;
        uf = 128.0f + Cb * 224.0f;
        vf = 128.0f + Cr * 224.0f;
    }
    auto q = [](float f) -> uint8_t {
        const float r = std::floor(f + 0.5f);
        return static_cast<uint8_t>(r < 0.0f ? 0.0f : (r > 255.0f ? 255.0f : r));
    };
    return Ayuv8{q(yf), q(uf), q(vf)};
}

winrt::com_ptr<ID3D11Texture2D> CreateBgraSource(ID3D11Device* device, const std::vector<Rgb8>& pixels) {
    const int width = static_cast<int>(pixels.size());
    std::vector<uint32_t> bgra(static_cast<size_t>(width));
    for (int i = 0; i < width; ++i) {
        // B8G8R8A8_UNORM memory order [B,G,R,A]; little-endian word.
        bgra[static_cast<size_t>(i)] = static_cast<uint32_t>(pixels[static_cast<size_t>(i)].b) |
                                       (static_cast<uint32_t>(pixels[static_cast<size_t>(i)].g) << 8) |
                                       (static_cast<uint32_t>(pixels[static_cast<size_t>(i)].r) << 16) | (0xFFu << 24);
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = bgra.data();
    init.SysMemPitch = static_cast<UINT>(width * 4);
    winrt::com_ptr<ID3D11Texture2D> tex;
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, &init, tex.put())));
    return tex;
}

winrt::com_ptr<ID3D11Texture2D> CreateAyuvTarget(ID3D11Device* device, int width) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    // R8G8B8A8_UINT: exact integer UAV writes; byte layout matches AYUV. (WARP does
    // not universally allow a UAV over a DXGI_FORMAT_AYUV resource; the real encode
    // path allocates AYUV, viewed as R8G8B8A8_UINT — same bytes.)
    desc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    winrt::com_ptr<ID3D11Texture2D> tex;
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, tex.put())));
    return tex;
}

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
    std::memcpy(out.data(), mapped.pData, out.size() * 4);
    context->Unmap(staging.get(), 0);
    return out;
}

std::vector<Ayuv8> ConvertRow(D3DTestDevice& d3d, const std::vector<Rgb8>& pixels, bool full_range) {
    const int width = static_cast<int>(pixels.size());
    auto src = CreateBgraSource(d3d.device.get(), pixels);
    auto dst = CreateAyuvTarget(d3d.device.get(), width);
    RgbToAyuvConverter conv;
    std::string err;
    EXPECT_TRUE(conv.Init(d3d.device.get(), d3d.context.get(), static_cast<UINT>(width), 1, full_range, err)) << err;
    EXPECT_TRUE(conv.Convert(src.get(), dst.get(), err)) << err;
    const auto texels = Read32(d3d.device.get(), d3d.context.get(), dst.get());
    std::vector<Ayuv8> out(texels.size());
    for (size_t i = 0; i < texels.size(); ++i) {
        const uint32_t t = texels[i];
        const uint8_t v = static_cast<uint8_t>(t & 0xFFu);
        const uint8_t u = static_cast<uint8_t>((t >> 8) & 0xFFu);
        const uint8_t y = static_cast<uint8_t>((t >> 16) & 0xFFu);
        out[i] = Ayuv8{y, u, v};
    }
    return out;
}

const std::vector<Rgb8> kSwatches = {
    {0, 0, 0},       {255, 255, 255}, {255, 0, 0},    {0, 255, 0},    {0, 0, 255},
    {128, 128, 128}, {200, 50, 100},  {16, 200, 240}, {73, 145, 220}, {255, 128, 0},
};

// --- limited/studio range matches the CPU reference bit-for-bit ---------------

TEST(GpuRgbToAyuv, LimitedRangeMatchesReference) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);

    const auto got = ConvertRow(d3d, kSwatches, /*full_range=*/false);
    ASSERT_EQ(got.size(), kSwatches.size());
    for (size_t i = 0; i < kSwatches.size(); ++i) {
        const Ayuv8 want = RefRgbToAyuv(kSwatches[i], false);
        EXPECT_NEAR(got[i].y, want.y, 1) << "Y at swatch " << i;
        EXPECT_NEAR(got[i].u, want.u, 1) << "U at swatch " << i;
        EXPECT_NEAR(got[i].v, want.v, 1) << "V at swatch " << i;
    }
}

// --- full range matches the CPU reference bit-for-bit -------------------------

TEST(GpuRgbToAyuv, FullRangeMatchesReference) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);

    const auto got = ConvertRow(d3d, kSwatches, /*full_range=*/true);
    ASSERT_EQ(got.size(), kSwatches.size());
    for (size_t i = 0; i < kSwatches.size(); ++i) {
        const Ayuv8 want = RefRgbToAyuv(kSwatches[i], true);
        EXPECT_NEAR(got[i].y, want.y, 1) << "Y at swatch " << i;
        EXPECT_NEAR(got[i].u, want.u, 1) << "U at swatch " << i;
        EXPECT_NEAR(got[i].v, want.v, 1) << "V at swatch " << i;
    }
}

// --- documented anchor values (exact) ----------------------------------------

TEST(GpuRgbToAyuv, LimitedRangeAnchors) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);

    const std::vector<Rgb8> pixels = {{0, 0, 0}, {255, 255, 255}, {128, 128, 128}};
    const auto got = ConvertRow(d3d, pixels, /*full_range=*/false);
    ASSERT_EQ(got.size(), 3u);

    // Black -> studio luma floor 16, neutral chroma 128.
    EXPECT_EQ(got[0].y, 16);
    EXPECT_EQ(got[0].u, 128);
    EXPECT_EQ(got[0].v, 128);
    // White -> studio luma ceiling 235, neutral chroma 128.
    EXPECT_EQ(got[1].y, 235);
    EXPECT_EQ(got[1].u, 128);
    EXPECT_EQ(got[1].v, 128);
    // Neutral grey -> chroma stays 128 (achromatic).
    EXPECT_EQ(got[2].u, 128);
    EXPECT_EQ(got[2].v, 128);
}

TEST(GpuRgbToAyuv, FullRangeBlackWhiteAnchors) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);

    const std::vector<Rgb8> pixels = {{0, 0, 0}, {255, 255, 255}};
    const auto got = ConvertRow(d3d, pixels, /*full_range=*/true);
    ASSERT_EQ(got.size(), 2u);

    // Full range spans 0..255.
    EXPECT_EQ(got[0].y, 0);
    EXPECT_EQ(got[0].u, 128);
    EXPECT_EQ(got[0].v, 128);
    EXPECT_EQ(got[1].y, 255);
    EXPECT_EQ(got[1].u, 128);
    EXPECT_EQ(got[1].v, 128);
}

} // namespace
