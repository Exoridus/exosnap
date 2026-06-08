#include <gtest/gtest.h>

#include "gpu_compositor.h"
#include "session_internal.h"

#include <d3d11.h>
#include <winrt/base.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

namespace {

using recorder_core::GpuCompositor;
using recorder_core::SessionState;
using recorder_core::WebcamOverlayLive;
using recorder_core::WebcamPixelRect;

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

winrt::com_ptr<ID3D11Texture2D> CreateTexture(ID3D11Device* device, int width, int height,
                                              const std::vector<uint8_t>& bgra) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = bgra.data();
    init.SysMemPitch = static_cast<UINT>(width * 4);

    winrt::com_ptr<ID3D11Texture2D> texture;
    const HRESULT hr = device->CreateTexture2D(&desc, &init, texture.put());
    EXPECT_TRUE(SUCCEEDED(hr));
    return texture;
}

std::vector<uint8_t> ReadTexture(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
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

    std::vector<uint8_t> out(static_cast<size_t>(desc.Width) * desc.Height * 4);
    for (UINT row = 0; row < desc.Height; ++row) {
        const auto* src = static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch;
        auto* dst = out.data() + static_cast<size_t>(row) * desc.Width * 4;
        std::memcpy(dst, src, static_cast<size_t>(desc.Width) * 4);
    }
    context->Unmap(staging.get(), 0);
    return out;
}

void ExpectPixelNear(const std::vector<uint8_t>& pixels, int width, int x, int y, uint8_t b, uint8_t g, uint8_t r,
                     uint8_t a, int tolerance = 2) {
    const size_t off = (static_cast<size_t>(y) * width + x) * 4;
    EXPECT_NEAR(static_cast<int>(pixels[off + 0]), static_cast<int>(b), tolerance);
    EXPECT_NEAR(static_cast<int>(pixels[off + 1]), static_cast<int>(g), tolerance);
    EXPECT_NEAR(static_cast<int>(pixels[off + 2]), static_cast<int>(r), tolerance);
    EXPECT_NEAR(static_cast<int>(pixels[off + 3]), static_cast<int>(a), tolerance);
}

std::vector<uint8_t> SolidBgra(int width, int height, uint8_t b, uint8_t g, uint8_t r, uint8_t a = 255) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (int i = 0; i < width * height; ++i) {
        pixels[static_cast<size_t>(i) * 4 + 0] = b;
        pixels[static_cast<size_t>(i) * 4 + 1] = g;
        pixels[static_cast<size_t>(i) * 4 + 2] = r;
        pixels[static_cast<size_t>(i) * 4 + 3] = a;
    }
    return pixels;
}

TEST(GpuCompositorTest, InitAndOpaquePaste) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);

    GpuCompositor compositor;
    std::string err;
    ASSERT_TRUE(compositor.Init(d3d.device.get(), d3d.context.get(), 4, 4, err)) << err;

    auto background = CreateTexture(d3d.device.get(), 4, 4, SolidBgra(4, 4, 10, 20, 30));
    ASSERT_TRUE(compositor.BeginFrame(background.get(), err)) << err;

    WebcamPixelRect rect{1, 1, 2, 2};
    auto webcam = SolidBgra(1, 1, 100, 110, 120);
    GpuCompositor::ChromaKeyParams chroma;
    ASSERT_TRUE(compositor.DrawWebcam(webcam.data(), 1, 1, rect, false, chroma, err)) << err;

    const auto pixels = ReadTexture(d3d.device.get(), d3d.context.get(), compositor.Result());
    ExpectPixelNear(pixels, 4, 0, 0, 10, 20, 30, 255);
    ExpectPixelNear(pixels, 4, 1, 1, 100, 110, 120, 255);
    ExpectPixelNear(pixels, 4, 2, 2, 100, 110, 120, 255);
}

TEST(GpuCompositorTest, MirrorFlipsHorizontallyOnly) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);

    GpuCompositor compositor;
    std::string err;
    ASSERT_TRUE(compositor.Init(d3d.device.get(), d3d.context.get(), 2, 1, err)) << err;

    auto background = CreateTexture(d3d.device.get(), 2, 1, SolidBgra(2, 1, 0, 0, 0));
    ASSERT_TRUE(compositor.BeginFrame(background.get(), err)) << err;

    std::vector<uint8_t> webcam = {
        0,   0, 255, 255, // red left
        255, 0, 0,   255, // blue right
    };
    GpuCompositor::ChromaKeyParams chroma;
    ASSERT_TRUE(compositor.DrawWebcam(webcam.data(), 2, 1, WebcamPixelRect{0, 0, 2, 1}, true, chroma, err)) << err;

    const auto pixels = ReadTexture(d3d.device.get(), d3d.context.get(), compositor.Result());
    ExpectPixelNear(pixels, 2, 0, 0, 255, 0, 0, 255);
    ExpectPixelNear(pixels, 2, 1, 0, 0, 0, 255, 255);
}

TEST(GpuCompositorTest, ChromaKeyMakesKeyColorTransparent) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);

    GpuCompositor compositor;
    std::string err;
    ASSERT_TRUE(compositor.Init(d3d.device.get(), d3d.context.get(), 2, 1, err)) << err;

    auto background = CreateTexture(d3d.device.get(), 2, 1, SolidBgra(2, 1, 10, 20, 30));
    ASSERT_TRUE(compositor.BeginFrame(background.get(), err)) << err;

    std::vector<uint8_t> webcam = {
        64, 177, 0,   255, // key color: r=0, g=177, b=64
        0,  0,   255, 255, // red
    };
    GpuCompositor::ChromaKeyParams chroma;
    chroma.enabled = true;
    chroma.r = 0;
    chroma.g = 177;
    chroma.b = 64;
    chroma.tolerance = 0.01f;
    chroma.softness = 0.01f;
    ASSERT_TRUE(compositor.DrawWebcam(webcam.data(), 2, 1, WebcamPixelRect{0, 0, 2, 1}, false, chroma, err)) << err;

    const auto pixels = ReadTexture(d3d.device.get(), d3d.context.get(), compositor.Result());
    ExpectPixelNear(pixels, 2, 0, 0, 10, 20, 30, 255);
    ExpectPixelNear(pixels, 2, 1, 0, 0, 0, 255, 255);
}

TEST(GpuCompositorTest, CursorUsesSourceAlpha) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);

    GpuCompositor compositor;
    std::string err;
    ASSERT_TRUE(compositor.Init(d3d.device.get(), d3d.context.get(), 1, 1, err)) << err;

    auto background = CreateTexture(d3d.device.get(), 1, 1, SolidBgra(1, 1, 0, 0, 0));
    ASSERT_TRUE(compositor.BeginFrame(background.get(), err)) << err;

    std::vector<uint8_t> cursor = {255, 255, 255, 128};
    ASSERT_TRUE(compositor.DrawCursor(cursor.data(), 1, 1, WebcamPixelRect{0, 0, 1, 1}, err)) << err;

    const auto pixels = ReadTexture(d3d.device.get(), d3d.context.get(), compositor.Result());
    ExpectPixelNear(pixels, 1, 0, 0, 128, 128, 128, 255);
}

TEST(SessionStateWebcamOverlayLiveTest, SeedUpdateAndSnapshotSanitizeLiveOverlay) {
    SessionState state;
    state.config.webcam.enabled = true;
    state.config.webcam.overlay_x_norm = 0.90f;
    state.config.webcam.overlay_y_norm = 0.25f;
    state.config.webcam.overlay_w_norm = 0.50f;
    state.config.webcam.overlay_h_norm = 0.25f;
    state.config.webcam.mirror = true;
    state.SeedWebcamOverlayFromConfig();

    WebcamOverlayLive seeded = state.SnapshotWebcamOverlay();
    EXPECT_TRUE(seeded.enabled);
    EXPECT_TRUE(seeded.mirror);
    EXPECT_LE(seeded.overlay_x_norm + seeded.overlay_w_norm, 1.0f);

    WebcamOverlayLive live;
    live.enabled = false;
    live.overlay_x_norm = std::nanf("");
    live.overlay_y_norm = -1.0f;
    live.overlay_w_norm = 2.0f;
    live.overlay_h_norm = 0.0f;
    live.chroma_tolerance = std::nanf("");
    live.chroma_softness = 2.0f;
    state.UpdateWebcamOverlay(live);

    const WebcamOverlayLive updated = state.SnapshotWebcamOverlay();
    EXPECT_FALSE(updated.enabled);
    EXPECT_FLOAT_EQ(updated.overlay_x_norm, 0.0f);
    EXPECT_FLOAT_EQ(updated.overlay_y_norm, 0.0f);
    EXPECT_FLOAT_EQ(updated.overlay_w_norm, 1.0f);
    EXPECT_FLOAT_EQ(updated.overlay_h_norm, recorder_core::WebcamPlacement::kMinSize);
    EXPECT_FLOAT_EQ(updated.chroma_tolerance, 0.30f);
    EXPECT_FLOAT_EQ(updated.chroma_softness, 1.0f);
}

} // namespace
