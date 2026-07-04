#include <gtest/gtest.h>

#include "dxgi_od_capture_src.h"
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

TEST(GpuCompositorTest, ChromaKey_RedForeground_NotKeyed) {
    // A red pixel in a green-keyed scene must NOT be transparent: YCbCr distance
    // between pure red and pure green is large, so red passes through opaque.
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);

    GpuCompositor compositor;
    std::string err;
    ASSERT_TRUE(compositor.Init(d3d.device.get(), d3d.context.get(), 2, 1, err)) << err;

    auto background = CreateTexture(d3d.device.get(), 2, 1, SolidBgra(2, 1, 50, 50, 50));
    ASSERT_TRUE(compositor.BeginFrame(background.get(), err)) << err;

    std::vector<uint8_t> webcam = {
        0, 0,   255, 255, // BGRA: pure red — key should NOT remove this
        0, 255, 0,   255, // BGRA: pure green — key SHOULD remove this
    };
    GpuCompositor::ChromaKeyParams chroma;
    chroma.enabled = true;
    chroma.r = 0;
    chroma.g = 255;
    chroma.b = 0;
    chroma.tolerance = 0.40f;
    chroma.softness = 0.10f;
    chroma.spill_reduction = 0.0f;
    ASSERT_TRUE(compositor.DrawWebcam(webcam.data(), 2, 1, WebcamPixelRect{0, 0, 2, 1}, false, chroma, err)) << err;

    const auto pixels = ReadTexture(d3d.device.get(), d3d.context.get(), compositor.Result());
    // Red pixel: opaque — webcam red must dominate over dark background
    ExpectPixelNear(pixels, 2, 0, 0, 0, 0, 255, 255, 10);
    // Green pixel: transparent — background shines through
    ExpectPixelNear(pixels, 2, 1, 0, 50, 50, 50, 255, 10);
}

TEST(GpuCompositorTest, SpillReduction_ReducesGreenTintOnSemiTransparentEdge) {
    // An edge pixel that is semi-transparent (neither fully key nor fully opaque)
    // should have its green spill reduced when spill_reduction > 0.
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);

    GpuCompositor compositor;
    std::string err;
    ASSERT_TRUE(compositor.Init(d3d.device.get(), d3d.context.get(), 1, 1, err)) << err;

    // Background: dark
    auto background = CreateTexture(d3d.device.get(), 1, 1, SolidBgra(1, 1, 20, 20, 20));
    ASSERT_TRUE(compositor.BeginFrame(background.get(), err)) << err;

    // Slightly-off-key green pixel (sits in the soft transition band): R=20,G=230,B=20
    std::vector<uint8_t> webcam = {20, 230, 20, 255};

    // tolerance=0.05, softness=0.10: YCbCr dist of this pixel ≈ 0.094,
    // which falls in the soft zone [0.05, 0.15] → semi-transparent, so spill runs.
    GpuCompositor::ChromaKeyParams chroma_no_spill;
    chroma_no_spill.enabled = true;
    chroma_no_spill.r = 0;
    chroma_no_spill.g = 255;
    chroma_no_spill.b = 0;
    chroma_no_spill.tolerance = 0.05f;
    chroma_no_spill.softness = 0.10f;
    chroma_no_spill.spill_reduction = 0.0f;
    ASSERT_TRUE(compositor.DrawWebcam(webcam.data(), 1, 1, WebcamPixelRect{0, 0, 1, 1}, false, chroma_no_spill, err))
        << err;
    const auto pixels_no_spill = ReadTexture(d3d.device.get(), d3d.context.get(), compositor.Result());

    // Reset and draw with spill_reduction=1 (maximum)
    ASSERT_TRUE(compositor.BeginFrame(background.get(), err)) << err;
    GpuCompositor::ChromaKeyParams chroma_full_spill = chroma_no_spill;
    chroma_full_spill.spill_reduction = 1.0f;
    ASSERT_TRUE(compositor.DrawWebcam(webcam.data(), 1, 1, WebcamPixelRect{0, 0, 1, 1}, false, chroma_full_spill, err))
        << err;
    const auto pixels_full_spill = ReadTexture(d3d.device.get(), d3d.context.get(), compositor.Result());

    // Green channel should be lower with spill reduction than without
    const int g_no_spill = static_cast<int>(pixels_no_spill[1]);
    const int g_full_spill = static_cast<int>(pixels_full_spill[1]);
    EXPECT_LT(g_full_spill, g_no_spill) << "spill_reduction=1 must reduce green channel vs spill_reduction=0";
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

// ---------------------------------------------------------------------------
// 10 bpc SDR desktop support (fix/od-10bit-desktop)
// ---------------------------------------------------------------------------

// R10G10B10A2_UNORM texel: R bits 0-9, G bits 10-19, B bits 20-29, A bits 30-31.
uint32_t PackR10G10B10A2(uint32_t r10, uint32_t g10, uint32_t b10, uint32_t a2) {
    return (r10 & 0x3FFu) | ((g10 & 0x3FFu) << 10) | ((b10 & 0x3FFu) << 20) | ((a2 & 0x3u) << 30);
}

winrt::com_ptr<ID3D11Texture2D> CreateTexture10Bit(ID3D11Device* device, int width, int height,
                                                   const std::vector<uint32_t>& texels) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = texels.data();
    init.SysMemPitch = static_cast<UINT>(width * 4);

    winrt::com_ptr<ID3D11Texture2D> texture;
    const HRESULT hr = device->CreateTexture2D(&desc, &init, texture.put());
    EXPECT_TRUE(SUCCEEDED(hr));
    return texture;
}

std::vector<uint32_t> ReadTexture10Bit(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
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
    for (UINT row = 0; row < desc.Height; ++row) {
        const auto* src = reinterpret_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch;
        std::memcpy(out.data() + static_cast<size_t>(row) * desc.Width, src, static_cast<size_t>(desc.Width) * 4);
    }
    context->Unmap(staging.get(), 0);
    return out;
}

void ExpectTexel10Near(const std::vector<uint32_t>& texels, int width, int x, int y, uint32_t r10, uint32_t g10,
                       uint32_t b10, int tolerance = 4) {
    const uint32_t v = texels[static_cast<size_t>(y) * width + x];
    EXPECT_NEAR(static_cast<int>(v & 0x3FFu), static_cast<int>(r10), tolerance);
    EXPECT_NEAR(static_cast<int>((v >> 10) & 0x3FFu), static_cast<int>(g10), tolerance);
    EXPECT_NEAR(static_cast<int>((v >> 20) & 0x3FFu), static_cast<int>(b10), tolerance);
}

TEST(OdCaptureFormatPolicyTest, SupportedFormats) {
    EXPECT_TRUE(recorder_core::IsSupportedOdCaptureFormat(DXGI_FORMAT_B8G8R8A8_UNORM));
    EXPECT_TRUE(recorder_core::IsSupportedOdCaptureFormat(DXGI_FORMAT_R10G10B10A2_UNORM));
    // HDR/FP16 is now a consumable input (tone-mapped to SDR BT.709 before the
    // VideoProcessor). Per-HDR-mode handling is covered by ResolveOdCaptureMode.
    EXPECT_TRUE(recorder_core::IsSupportedOdCaptureFormat(DXGI_FORMAT_R16G16B16A16_FLOAT));
    EXPECT_FALSE(recorder_core::IsSupportedOdCaptureFormat(DXGI_FORMAT_R8G8B8A8_UNORM));
    EXPECT_FALSE(recorder_core::IsSupportedOdCaptureFormat(DXGI_FORMAT_NV12));
    EXPECT_FALSE(recorder_core::IsSupportedOdCaptureFormat(DXGI_FORMAT_UNKNOWN));
}

TEST(OdCaptureFormatPolicyTest, FormatNames) {
    char buf[32];
    EXPECT_STREQ("B8G8R8A8_UNORM (8-bit SDR)",
                 recorder_core::OdCaptureFormatName(DXGI_FORMAT_B8G8R8A8_UNORM, buf, sizeof(buf)));
    EXPECT_STREQ("R10G10B10A2_UNORM (10 bpc SDR)",
                 recorder_core::OdCaptureFormatName(DXGI_FORMAT_R10G10B10A2_UNORM, buf, sizeof(buf)));
    EXPECT_STREQ("R16G16B16A16_FLOAT (HDR/Advanced Color)",
                 recorder_core::OdCaptureFormatName(DXGI_FORMAT_R16G16B16A16_FLOAT, buf, sizeof(buf)));
    // Unknown formats fall back to a numeric rendering (never crashes/nullptr).
    EXPECT_STREQ("DXGI_FORMAT(28)", recorder_core::OdCaptureFormatName(DXGI_FORMAT_R8G8B8A8_UNORM, buf, sizeof(buf)));
}

TEST(GpuCompositorTest, TenBit_InitAndOpaquePaste) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);

    GpuCompositor compositor;
    std::string err;
    ASSERT_TRUE(compositor.Init(d3d.device.get(), d3d.context.get(), 4, 4, err, DXGI_FORMAT_R10G10B10A2_UNORM)) << err;

    // Background: 10-bit values that have NO exact 8-bit representation
    // (verifies precision is preserved through BeginFrame's CopyResource).
    const uint32_t bg = PackR10G10B10A2(513, 257, 771, 3);
    auto background = CreateTexture10Bit(d3d.device.get(), 4, 4, std::vector<uint32_t>(16, bg));
    ASSERT_TRUE(compositor.BeginFrame(background.get(), err)) << err;

    // Webcam upload stays BGRA8; the shader samples it normalized and the
    // 10-bit RTV re-quantizes: v8/255*1023.
    WebcamPixelRect rect{1, 1, 2, 2};
    auto webcam = SolidBgra(1, 1, 100, 110, 120); // b, g, r
    GpuCompositor::ChromaKeyParams chroma;
    ASSERT_TRUE(compositor.DrawWebcam(webcam.data(), 1, 1, rect, false, chroma, err)) << err;

    const auto texels = ReadTexture10Bit(d3d.device.get(), d3d.context.get(), compositor.Result());
    // Untouched background pixel keeps its exact 10-bit value.
    ExpectTexel10Near(texels, 4, 0, 0, 513, 257, 771, 0);
    // Webcam pixel: 8-bit -> normalized -> 10-bit (r=120 -> 481, g=110 -> 441, b=100 -> 401).
    ExpectTexel10Near(texels, 4, 1, 1, 481, 441, 401);
    ExpectTexel10Near(texels, 4, 2, 2, 481, 441, 401);
}

TEST(GpuCompositorTest, TenBit_CursorAlphaBlend) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);

    GpuCompositor compositor;
    std::string err;
    ASSERT_TRUE(compositor.Init(d3d.device.get(), d3d.context.get(), 1, 1, err, DXGI_FORMAT_R10G10B10A2_UNORM)) << err;

    auto background = CreateTexture10Bit(d3d.device.get(), 1, 1, {PackR10G10B10A2(0, 0, 0, 3)});
    ASSERT_TRUE(compositor.BeginFrame(background.get(), err)) << err;

    // White cursor at alpha 128 over black: result ~= 128/255 * 1023 ~= 514.
    std::vector<uint8_t> cursor = {255, 255, 255, 128};
    ASSERT_TRUE(compositor.DrawCursor(cursor.data(), 1, 1, WebcamPixelRect{0, 0, 1, 1}, err)) << err;

    const auto texels = ReadTexture10Bit(d3d.device.get(), d3d.context.get(), compositor.Result());
    ExpectTexel10Near(texels, 1, 0, 0, 514, 514, 514, 6);
}

TEST(GpuCompositorTest, TenBit_BeginFrameRejectsFormatMismatch) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);

    // Compositor negotiated for a 10-bit source must reject BGRA8 backgrounds…
    GpuCompositor compositor10;
    std::string err;
    ASSERT_TRUE(compositor10.Init(d3d.device.get(), d3d.context.get(), 2, 2, err, DXGI_FORMAT_R10G10B10A2_UNORM))
        << err;
    auto bgra_background = CreateTexture(d3d.device.get(), 2, 2, SolidBgra(2, 2, 1, 2, 3));
    err.clear();
    EXPECT_FALSE(compositor10.BeginFrame(bgra_background.get(), err));
    EXPECT_FALSE(err.empty());

    // …and the BGRA8 default must reject 10-bit backgrounds (silent CopyResource
    // no-ops are exactly the failure mode this guards against).
    GpuCompositor compositor8;
    err.clear();
    ASSERT_TRUE(compositor8.Init(d3d.device.get(), d3d.context.get(), 2, 2, err)) << err;
    auto ten_bit_background =
        CreateTexture10Bit(d3d.device.get(), 2, 2, std::vector<uint32_t>(4, PackR10G10B10A2(1, 2, 3, 3)));
    err.clear();
    EXPECT_FALSE(compositor8.BeginFrame(ten_bit_background.get(), err));
    EXPECT_FALSE(err.empty());
}

TEST(GpuCompositorTest, InitRejectsUnsupportedRenderFormat) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);

    GpuCompositor compositor;
    std::string err;
    EXPECT_FALSE(compositor.Init(d3d.device.get(), d3d.context.get(), 2, 2, err, DXGI_FORMAT_R16G16B16A16_FLOAT));
    EXPECT_FALSE(err.empty());
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
    EXPECT_FLOAT_EQ(updated.chroma_tolerance, 0.40f); // NaN → fallback default
    EXPECT_FLOAT_EQ(updated.chroma_softness, 1.0f);   // 2.0 clamped to 1.0
    EXPECT_FLOAT_EQ(updated.chroma_spill_reduction, 0.30f);
}

} // namespace
