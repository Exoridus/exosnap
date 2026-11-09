#include <exosnap/engine/gpu_surface_inventory.h>

#include <gtest/gtest.h>
#include <wrl/client.h>

#include <array>

namespace {

using namespace exosnap::engine;
using Microsoft::WRL::ComPtr;

D3D11_TEXTURE2D_DESC TextureDesc(DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 32;
    desc.Height = 16;
    desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return desc;
}

ComPtr<ID3D11Device> CreateWarpDevice() {
    ComPtr<ID3D11Device> device;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    EXPECT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 1, D3D11_SDK_VERSION,
                                            device.GetAddressOf(), nullptr, nullptr)));
    return device;
}

struct FormatBytes {
    DXGI_FORMAT format;
    uint64_t bytes;
};

constexpr std::array<FormatBytes, 4> kUintFormats{{{DXGI_FORMAT_R8_UINT, 512},
                                                   {DXGI_FORMAT_R8G8_UINT, 1024},
                                                   {DXGI_FORMAT_R16_UINT, 1024},
                                                   {DXGI_FORMAT_R8G8B8A8_UINT, 2048}}};

TEST(GpuSurfaceInventory, CountsIntegerUploadFormats) {
    for (const auto& [format, bytes] : kUintFormats) {
        SCOPED_TRACE(static_cast<int>(format));
        EXPECT_EQ(LogicalSurfaceBytes(TextureDesc(format)), bytes);
    }
}

TEST(GpuSurfaceInventory, PreservesKnownColorAndPlanarEstimates) {
    constexpr std::array<FormatBytes, 10> formats{{{DXGI_FORMAT_R16G16B16A16_FLOAT, 4096},
                                                   {DXGI_FORMAT_B8G8R8A8_UNORM, 2048},
                                                   {DXGI_FORMAT_R8G8B8A8_UNORM, 2048},
                                                   {DXGI_FORMAT_R10G10B10A2_UNORM, 2048},
                                                   {DXGI_FORMAT_AYUV, 2048},
                                                   {DXGI_FORMAT_NV12, 768},
                                                   {DXGI_FORMAT_P010, 1536},
                                                   {DXGI_FORMAT_R8_UNORM, 512},
                                                   {DXGI_FORMAT_R16_UNORM, 1024},
                                                   {DXGI_FORMAT_R8G8_UNORM, 1024}}};
    for (const auto& [format, bytes] : formats) {
        SCOPED_TRACE(static_cast<int>(format));
        EXPECT_EQ(LogicalSurfaceBytes(TextureDesc(format)), bytes);
    }
    EXPECT_EQ(LogicalSurfaceBytes(TextureDesc(DXGI_FORMAT_R16G16_UNORM)), 2048u);
}

TEST(GpuSurfaceInventory, IncludesArraySlicesAndSamplesWithout32BitOverflow) {
    auto desc = TextureDesc(DXGI_FORMAT_R8G8B8A8_UINT);
    desc.ArraySize = 3;
    desc.SampleDesc.Count = 4;
    EXPECT_EQ(LogicalSurfaceBytes(desc), 24576u);
    desc = TextureDesc(DXGI_FORMAT_R16G16B16A16_FLOAT);
    desc.Width = desc.Height = 16384;
    desc.ArraySize = 16;
    EXPECT_EQ(LogicalSurfaceBytes(desc), 34359738368ULL);
}

TEST(GpuSurfaceInventory, UnsupportedFormatsAndMipChainsHaveNoByteEstimate) {
    EXPECT_EQ(LogicalSurfaceBytes(TextureDesc(DXGI_FORMAT_R32_FLOAT)), 0u);
    auto desc = TextureDesc(DXGI_FORMAT_R8_UNORM);
    desc.MipLevels = 2;
    EXPECT_EQ(LogicalSurfaceBytes(desc), 0u);
    desc.MipLevels = 0;
    EXPECT_EQ(LogicalSurfaceBytes(desc), 0u);
}

TEST(GpuSurfaceInventory, TracksIntegerTextureLifetimeThroughExternalReferences) {
    auto device = CreateWarpDevice();
    ASSERT_NE(device, nullptr);
    const auto before = ReadGpuSurfaceUsage(GpuSurfaceOwner::Editor);
    for (const auto& [format, bytes] : kUintFormats) {
        SCOPED_TRACE(static_cast<int>(format));
        const auto desc = TextureDesc(format);
        ComPtr<ID3D11Texture2D> texture;
        ASSERT_TRUE(SUCCEEDED(
            CreateTrackedTexture2D(device.Get(), &desc, nullptr, texture.GetAddressOf(), GpuSurfaceOwner::Editor)));
        auto held = texture;
        texture.Reset();
        EXPECT_EQ(ReadGpuSurfaceUsage(GpuSurfaceOwner::Editor).bytes, before.bytes + bytes);
        EXPECT_EQ(ReadGpuSurfaceUsage(GpuSurfaceOwner::Editor).surfaces, before.surfaces + 1);
        EXPECT_EQ(ReadGpuSurfaceUsage(GpuSurfaceOwner::Editor).unknown_byte_surfaces, before.unknown_byte_surfaces);
        held.Reset();
        EXPECT_EQ(ReadGpuSurfaceUsage(GpuSurfaceOwner::Editor).bytes, before.bytes);
        EXPECT_EQ(ReadGpuSurfaceUsage(GpuSurfaceOwner::Editor).surfaces, before.surfaces);
        EXPECT_EQ(ReadGpuSurfaceUsage(GpuSurfaceOwner::Editor).unknown_byte_surfaces, before.unknown_byte_surfaces);
    }
}

TEST(GpuSurfaceInventory, TracksUnknownByteTexturesThroughExternalReferences) {
    auto device = CreateWarpDevice();
    ASSERT_NE(device, nullptr);
    const auto before = ReadGpuSurfaceUsage(GpuSurfaceOwner::Editor);
    const auto desc = TextureDesc(DXGI_FORMAT_R32_FLOAT);
    ComPtr<ID3D11Texture2D> texture;
    ASSERT_TRUE(SUCCEEDED(
        CreateTrackedTexture2D(device.Get(), &desc, nullptr, texture.GetAddressOf(), GpuSurfaceOwner::Editor)));
    auto held = texture;
    texture.Reset();
    EXPECT_EQ(ReadGpuSurfaceUsage(GpuSurfaceOwner::Editor).bytes, before.bytes);
    EXPECT_EQ(ReadGpuSurfaceUsage(GpuSurfaceOwner::Editor).surfaces, before.surfaces + 1);
    EXPECT_EQ(ReadGpuSurfaceUsage(GpuSurfaceOwner::Editor).unknown_byte_surfaces, before.unknown_byte_surfaces + 1);
    held.Reset();
    EXPECT_EQ(ReadGpuSurfaceUsage(GpuSurfaceOwner::Editor).bytes, before.bytes);
    EXPECT_EQ(ReadGpuSurfaceUsage(GpuSurfaceOwner::Editor).surfaces, before.surfaces);
    EXPECT_EQ(ReadGpuSurfaceUsage(GpuSurfaceOwner::Editor).unknown_byte_surfaces, before.unknown_byte_surfaces);
}

} // namespace
