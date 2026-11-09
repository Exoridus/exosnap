#include "QuickPreviewRgbaConverter.h"

#include <exosnap/engine/gpu_surface_inventory.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using exosnap::engine::GpuSurfaceOwner;
using exosnap::engine::ReadGpuSurfaceUsage;
using exosnap::quick::QuickPreviewRgbaConverter;
using Microsoft::WRL::ComPtr;

struct Device {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

Device CreateWarpDevice() {
    Device result;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    EXPECT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 1, D3D11_SDK_VERSION,
                                            result.device.GetAddressOf(), nullptr, result.context.GetAddressOf())));
    return result;
}

ComPtr<ID3D11Texture2D> CreateTexture(ID3D11Device* device, DXGI_FORMAT format, UINT flags,
                                      std::array<uint8_t, 4> pixel = {}) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 2;
    desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.BindFlags = flags;
    std::vector<uint8_t> pixels;
    for (int i = 0; i < 4; ++i)
        pixels.insert(pixels.end(), pixel.begin(), pixel.end());
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = pixels.data();
    data.SysMemPitch = 8;
    ComPtr<ID3D11Texture2D> texture;
    EXPECT_TRUE(SUCCEEDED(exosnap::engine::CreateTrackedTexture2D(device, &desc, &data, texture.GetAddressOf(),
                                                                  GpuSurfaceOwner::Preview)));
    return texture;
}

std::vector<uint8_t> ReadPixels(const Device& d3d, ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(d3d.device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) {
        ADD_FAILURE() << "Could not allocate the readback fixture";
        return {};
    }
    d3d.context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(d3d.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        ADD_FAILURE() << "Could not map the readback fixture";
        return {};
    }
    std::vector<uint8_t> pixels;
    for (UINT row = 0; row < desc.Height; ++row) {
        const auto* first = static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch;
        pixels.insert(pixels.end(), first, first + desc.Width * 4);
    }
    d3d.context->Unmap(staging.Get(), 0);
    return pixels;
}

void ExpectRedFrame(const Device& d3d, ID3D11Texture2D* texture) {
    const auto pixels = ReadPixels(d3d, texture);
    ASSERT_EQ(pixels.size(), 16u);
    for (size_t offset = 0; offset < pixels.size(); offset += 4) {
        EXPECT_EQ(pixels[offset], 235);
        EXPECT_EQ(pixels[offset + 1], 19);
        EXPECT_EQ(pixels[offset + 2], 7);
        EXPECT_EQ(pixels[offset + 3], 255);
    }
}

TEST(PreviewRgbaConverter, RebindingReleasesPreviousTextureViews) {
    auto d3d = CreateWarpDevice();
    ASSERT_NE(d3d.device, nullptr);
    const auto before = ReadGpuSurfaceUsage(GpuSurfaceOwner::Preview);
    {
        QuickPreviewRgbaConverter converter;
        for (int i = 0; i < 16; ++i) {
            auto source = CreateTexture(d3d.device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
            auto target = CreateTexture(d3d.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
            std::string error;
            ASSERT_TRUE(
                converter.initialize(d3d.device.Get(), d3d.context.Get(), source.Get(), target.Get(), 2, 2, error))
                << error;
            source.Reset();
            target.Reset();
            EXPECT_EQ(ReadGpuSurfaceUsage(GpuSurfaceOwner::Preview).surfaces, before.surfaces + 2);
            EXPECT_EQ(ReadGpuSurfaceUsage(GpuSurfaceOwner::Preview).bytes, before.bytes + 32);
        }
    }
    EXPECT_EQ(ReadGpuSurfaceUsage(GpuSurfaceOwner::Preview).surfaces, before.surfaces);
    EXPECT_EQ(ReadGpuSurfaceUsage(GpuSurfaceOwner::Preview).bytes, before.bytes);
}

TEST(PreviewRgbaConverter, RebindingOnSameDeviceRetainsShaderObjects) {
    auto d3d = CreateWarpDevice();
    ASSERT_NE(d3d.device, nullptr);
    auto source =
        CreateTexture(d3d.device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, {7, 19, 235, 0});
    auto target = CreateTexture(d3d.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
    QuickPreviewRgbaConverter converter;
    std::string error;
    ASSERT_TRUE(converter.initialize(d3d.device.Get(), d3d.context.Get(), source.Get(), target.Get(), 2, 2, error));
    ASSERT_TRUE(converter.convert(error));
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    d3d.context->VSGetShader(vertex.GetAddressOf(), nullptr, nullptr);
    d3d.context->PSGetShader(pixel.GetAddressOf(), nullptr, nullptr);
    ASSERT_NE(vertex, nullptr);
    ASSERT_NE(pixel, nullptr);
    d3d.context->ClearState();
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(converter.initialize(d3d.device.Get(), d3d.context.Get(), source.Get(), target.Get(), 2, 2, error));
        ASSERT_TRUE(converter.convert(error));
        ComPtr<ID3D11VertexShader> rebound_vertex;
        ComPtr<ID3D11PixelShader> rebound_pixel;
        d3d.context->VSGetShader(rebound_vertex.GetAddressOf(), nullptr, nullptr);
        d3d.context->PSGetShader(rebound_pixel.GetAddressOf(), nullptr, nullptr);
        EXPECT_EQ(rebound_vertex.Get(), vertex.Get());
        EXPECT_EQ(rebound_pixel.Get(), pixel.Get());
        d3d.context->ClearState();
    }
    ExpectRedFrame(d3d, target.Get());
}

TEST(PreviewRgbaConverter, FailedTargetViewPreservesPreviousConfiguration) {
    auto d3d = CreateWarpDevice();
    ASSERT_NE(d3d.device, nullptr);
    auto source =
        CreateTexture(d3d.device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, {7, 19, 235, 0});
    auto target = CreateTexture(d3d.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
    QuickPreviewRgbaConverter converter;
    std::string error;
    ASSERT_TRUE(converter.initialize(d3d.device.Get(), d3d.context.Get(), source.Get(), target.Get(), 2, 2, error));
    auto replacement =
        CreateTexture(d3d.device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, {0, 255, 0, 0});
    auto invalid_target = CreateTexture(d3d.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    EXPECT_FALSE(converter.initialize(d3d.device.Get(), d3d.context.Get(), replacement.Get(), invalid_target.Get(), 1,
                                      1, error));
    ASSERT_TRUE(converter.convert(error));
    ExpectRedFrame(d3d, target.Get());
    d3d.context->ClearState();
}

TEST(PreviewRgbaConverter, DeviceReplacementReleasesPreviousTextureViews) {
    auto first = CreateWarpDevice();
    auto second = CreateWarpDevice();
    ASSERT_NE(first.device, nullptr);
    ASSERT_NE(second.device, nullptr);
    const auto before = ReadGpuSurfaceUsage(GpuSurfaceOwner::Preview);
    {
        QuickPreviewRgbaConverter converter;
        auto source = CreateTexture(first.device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        auto target = CreateTexture(first.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
        std::string error;
        ASSERT_TRUE(
            converter.initialize(first.device.Get(), first.context.Get(), source.Get(), target.Get(), 2, 2, error));
        source.Reset();
        target.Reset();
        source =
            CreateTexture(second.device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, {7, 19, 235, 0});
        target = CreateTexture(second.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
        ASSERT_TRUE(
            converter.initialize(second.device.Get(), second.context.Get(), source.Get(), target.Get(), 2, 2, error));
        EXPECT_EQ(ReadGpuSurfaceUsage(GpuSurfaceOwner::Preview).surfaces, before.surfaces + 2);
        ASSERT_TRUE(converter.convert(error));
        ExpectRedFrame(second, target.Get());
        second.context->ClearState();
    }
    second.context->Flush();
    EXPECT_EQ(ReadGpuSurfaceUsage(GpuSurfaceOwner::Preview).surfaces, before.surfaces);
}

TEST(PreviewRgbaConverter, FailedDeviceReplacementPreservesPreviousConfiguration) {
    auto first = CreateWarpDevice();
    auto second = CreateWarpDevice();
    ASSERT_NE(first.device, nullptr);
    ASSERT_NE(second.device, nullptr);
    auto source =
        CreateTexture(first.device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, {7, 19, 235, 0});
    auto target = CreateTexture(first.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
    QuickPreviewRgbaConverter converter;
    std::string error;
    ASSERT_TRUE(converter.initialize(first.device.Get(), first.context.Get(), source.Get(), target.Get(), 2, 2, error));
    auto replacement = CreateTexture(second.device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    auto invalid_target = CreateTexture(second.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    EXPECT_FALSE(converter.initialize(second.device.Get(), second.context.Get(), replacement.Get(),
                                      invalid_target.Get(), 2, 2, error));
    ASSERT_TRUE(converter.convert(error));
    ExpectRedFrame(first, target.Get());
    first.context->ClearState();
}

TEST(PreviewRgbaConverter, RejectsContextFromAnotherDeviceWithoutReplacingConfiguration) {
    auto first = CreateWarpDevice();
    auto second = CreateWarpDevice();
    ASSERT_NE(first.device, nullptr);
    ASSERT_NE(second.device, nullptr);
    auto source =
        CreateTexture(first.device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, {7, 19, 235, 0});
    auto target = CreateTexture(first.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
    QuickPreviewRgbaConverter converter;
    std::string error;
    ASSERT_TRUE(converter.initialize(first.device.Get(), first.context.Get(), source.Get(), target.Get(), 2, 2, error));
    EXPECT_FALSE(
        converter.initialize(first.device.Get(), second.context.Get(), source.Get(), target.Get(), 2, 2, error));
    ASSERT_TRUE(converter.convert(error));
    ExpectRedFrame(first, target.Get());
    first.context->ClearState();
    second.context->ClearState();
}

} // namespace
