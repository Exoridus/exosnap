// The overlay shader is shared by the recording compositor and the DXGI live
// preview. test_gpu_compositor.cpp exercises it through GpuCompositor; this file
// exercises it the way the *preview* does — compiled standalone against its own
// device, constant buffer and blend state, with no compositor in the picture.
//
// That is what keeps the two consumers honest: if the preview's usage of the
// shared constants ever drifts from the key mathematics, this test fails, not a
// user staring at a green fringe the recording does not have.

#include <recorder_core/overlay_shader.h>

#include <gtest/gtest.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <winrt/base.h>

#include <cmath>
#include <cstring>
#include <iterator>
#include <vector>

namespace recorder_core {
namespace {

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

struct Bgra {
    uint8_t b, g, r, a;
};

// A minimal renderer that mirrors DxgiPreviewRenderer's pipeline setup: the shared
// shader pair, a point sampler, one constant buffer, straight src-alpha blending.
class StandaloneOverlayPass {
  public:
    explicit StandaloneOverlayPass(D3DTestDevice& d3d) : device_(d3d.device.get()), context_(d3d.context.get()) {
        winrt::com_ptr<ID3DBlob> vs_blob, ps_blob, err_blob;
        EXPECT_TRUE(SUCCEEDED(D3DCompile(kOverlayVertexShaderSrc, std::strlen(kOverlayVertexShaderSrc), "vs", nullptr,
                                         nullptr, "main", "vs_5_0", 0, 0, vs_blob.put(), err_blob.put())));
        err_blob = nullptr;
        EXPECT_TRUE(SUCCEEDED(D3DCompile(kOverlayPixelShaderSrc, std::strlen(kOverlayPixelShaderSrc), "ps", nullptr,
                                         nullptr, "main", "ps_5_0", 0, 0, ps_blob.put(), err_blob.put())));
        EXPECT_TRUE(SUCCEEDED(
            device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, vs_.put())));
        EXPECT_TRUE(SUCCEEDED(
            device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, ps_.put())));

        // Point sampling: the assertions are about the key mathematics, not about
        // the bilinear filter the live paths use.
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        EXPECT_TRUE(SUCCEEDED(device_->CreateSamplerState(&sd, sampler_.put())));

        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        EXPECT_TRUE(SUCCEEDED(device_->CreateBlendState(&bd, blend_.put())));

        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(OverlayPixelConstants);
        cb.Usage = D3D11_USAGE_DEFAULT;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        EXPECT_TRUE(SUCCEEDED(device_->CreateBuffer(&cb, nullptr, constants_.put())));
    }

    // Composites `sprite` (one row) over a `background`-filled target of the same
    // width, and returns the resulting row.
    std::vector<Bgra> Composite(const std::vector<Bgra>& sprite, Bgra background, const OverlayPixelConstants& pc) {
        const UINT width = static_cast<UINT>(sprite.size());

        winrt::com_ptr<ID3D11Texture2D> src = MakeTexture(width, D3D11_BIND_SHADER_RESOURCE, sprite.data());
        winrt::com_ptr<ID3D11ShaderResourceView> srv;
        EXPECT_TRUE(SUCCEEDED(device_->CreateShaderResourceView(src.get(), nullptr, srv.put())));

        winrt::com_ptr<ID3D11Texture2D> rt = MakeTexture(width, D3D11_BIND_RENDER_TARGET, nullptr);
        winrt::com_ptr<ID3D11RenderTargetView> rtv;
        EXPECT_TRUE(SUCCEEDED(device_->CreateRenderTargetView(rt.get(), nullptr, rtv.put())));

        const float clear[4] = {static_cast<float>(background.r) / 255.0f, static_cast<float>(background.g) / 255.0f,
                                static_cast<float>(background.b) / 255.0f, static_cast<float>(background.a) / 255.0f};
        context_->ClearRenderTargetView(rtv.get(), clear);

        context_->UpdateSubresource(constants_.get(), 0, nullptr, &pc, 0, 0);

        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(width);
        vp.Height = 1.0f;
        vp.MaxDepth = 1.0f;

        ID3D11RenderTargetView* rtv_raw = rtv.get();
        ID3D11ShaderResourceView* srv_raw = srv.get();
        ID3D11SamplerState* samp_raw = sampler_.get();
        ID3D11Buffer* cb_raw = constants_.get();
        const float blend_factor[4] = {};

        context_->OMSetRenderTargets(1, &rtv_raw, nullptr);
        context_->OMSetBlendState(blend_.get(), blend_factor, 0xffffffff);
        context_->RSSetViewports(1, &vp);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.get(), nullptr, 0);
        context_->PSSetShader(ps_.get(), nullptr, 0);
        context_->PSSetSamplers(0, 1, &samp_raw);
        context_->PSSetConstantBuffers(0, 1, &cb_raw);
        context_->PSSetShaderResources(0, 1, &srv_raw);
        context_->Draw(3, 0);

        return ReadBack(rt.get(), width);
    }

  private:
    winrt::com_ptr<ID3D11Texture2D> MakeTexture(UINT width, UINT bind, const void* pixels) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = width;
        d.Height = 1;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = bind;

        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = pixels;
        init.SysMemPitch = width * 4;

        winrt::com_ptr<ID3D11Texture2D> tex;
        EXPECT_TRUE(SUCCEEDED(device_->CreateTexture2D(&d, pixels != nullptr ? &init : nullptr, tex.put())));
        return tex;
    }

    std::vector<Bgra> ReadBack(ID3D11Texture2D* texture, UINT width) {
        D3D11_TEXTURE2D_DESC d{};
        texture->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        winrt::com_ptr<ID3D11Texture2D> staging;
        EXPECT_TRUE(SUCCEEDED(device_->CreateTexture2D(&d, nullptr, staging.put())));
        context_->CopyResource(staging.get(), texture);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        EXPECT_TRUE(SUCCEEDED(context_->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
        std::vector<Bgra> out(width);
        std::memcpy(out.data(), mapped.pData, static_cast<size_t>(width) * 4);
        context_->Unmap(staging.get(), 0);
        return out;
    }

    ID3D11Device* device_;
    ID3D11DeviceContext* context_;
    winrt::com_ptr<ID3D11VertexShader> vs_;
    winrt::com_ptr<ID3D11PixelShader> ps_;
    winrt::com_ptr<ID3D11SamplerState> sampler_;
    winrt::com_ptr<ID3D11BlendState> blend_;
    winrt::com_ptr<ID3D11Buffer> constants_;
};

constexpr Bgra kGreen{0, 255, 0, 255};
constexpr Bgra kRed{0, 0, 255, 255};
constexpr Bgra kBlue{255, 0, 0, 255};
constexpr Bgra kWhite{255, 255, 255, 255};
constexpr Bgra kBlack{0, 0, 0, 255};

ChromaKeyParams GreenKey() {
    ChromaKeyParams key;
    key.enabled = true;
    key.r = 0;
    key.g = 255;
    key.b = 0;
    key.tolerance = 0.40f;
    key.softness = 0.15f;
    key.spill_reduction = 0.30f;
    return key;
}

void ExpectNear(const Bgra& actual, const Bgra& expected, int tolerance, const char* what) {
    EXPECT_NEAR(actual.b, expected.b, tolerance) << what << " (blue)";
    EXPECT_NEAR(actual.g, expected.g, tolerance) << what << " (green)";
    EXPECT_NEAR(actual.r, expected.r, tolerance) << what << " (red)";
}

// --- Constant packing (no GPU) ------------------------------------------------

TEST(OverlayShaderConstants, ModeFollowsChromaThenForceOpaque) {
    EXPECT_EQ(SelectOverlayMode(/*chroma_enabled=*/false, /*force_opaque=*/false), OverlayMode::Cursor);
    EXPECT_EQ(SelectOverlayMode(/*chroma_enabled=*/false, /*force_opaque=*/true), OverlayMode::Opaque);
    // An enabled key wins over force_opaque — otherwise a keyed webcam would render solid.
    EXPECT_EQ(SelectOverlayMode(/*chroma_enabled=*/true, /*force_opaque=*/true), OverlayMode::Chroma);
}

TEST(OverlayShaderConstants, PacksKeyColorMirrorAndOpacity) {
    const OverlayPixelConstants pc = MakeOverlayPixelConstants(GreenKey(), /*mirror=*/true, /*force_opaque=*/true,
                                                               /*opacity=*/0.5f, /*hdr_linear=*/false, 1.0f);
    EXPECT_FLOAT_EQ(pc.key_color[0], 0.0f);
    EXPECT_FLOAT_EQ(pc.key_color[1], 1.0f);
    EXPECT_FLOAT_EQ(pc.key_color[2], 0.0f);
    EXPECT_FLOAT_EQ(pc.key_color[3], 0.40f);
    EXPECT_FLOAT_EQ(pc.params[0], 1.0f); // mirror
    EXPECT_FLOAT_EQ(pc.params[1], 1.0f); // mode == Chroma
    EXPECT_FLOAT_EQ(pc.params[2], 0.30f);
    EXPECT_FLOAT_EQ(pc.params[3], 0.15f);
    EXPECT_FLOAT_EQ(pc.params2[0], 0.0f);
    EXPECT_FLOAT_EQ(pc.params2[2], 0.5f);
}

TEST(OverlayShaderConstants, NonFiniteOpacityFallsBackToOpaque) {
    const ChromaKeyParams none;
    const OverlayPixelConstants nan_pc = MakeOverlayPixelConstants(none, false, true, std::nanf(""), false, 1.0f);
    EXPECT_FLOAT_EQ(nan_pc.params2[2], 1.0f);

    const OverlayPixelConstants over = MakeOverlayPixelConstants(none, false, true, 4.0f, false, 1.0f);
    EXPECT_FLOAT_EQ(over.params2[2], 1.0f);

    const OverlayPixelConstants under = MakeOverlayPixelConstants(none, false, true, -1.0f, false, 1.0f);
    EXPECT_FLOAT_EQ(under.params2[2], 0.0f);
}

// --- The shader itself, driven exactly as the preview drives it ---------------

TEST(OverlayShaderWarp, ChromaKeyDropsKeyColorAndKeepsTheRest) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);
    StandaloneOverlayPass pass(d3d);

    const OverlayPixelConstants pc = MakeOverlayPixelConstants(GreenKey(), /*mirror=*/false, /*force_opaque=*/true,
                                                               /*opacity=*/1.0f, /*hdr_linear=*/false, 1.0f);
    const std::vector<Bgra> out = pass.Composite({kGreen, kRed}, kBlue, pc);

    ExpectNear(out[0], kBlue, 2, "keyed green reveals the background");
    ExpectNear(out[1], kRed, 2, "non-key colour survives");
}

TEST(OverlayShaderWarp, MirrorFlipsHorizontallyWithoutTouchingTheTexture) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);
    StandaloneOverlayPass pass(d3d);

    ChromaKeyParams none;
    const OverlayPixelConstants pc = MakeOverlayPixelConstants(none, /*mirror=*/true, /*force_opaque=*/true,
                                                               /*opacity=*/1.0f, /*hdr_linear=*/false, 1.0f);
    // Same upload as the unmirrored case; only params.x differs.
    const std::vector<Bgra> out = pass.Composite({kRed, kBlue}, kBlack, pc);

    ExpectNear(out[0], kBlue, 2, "mirrored: right texel lands left");
    ExpectNear(out[1], kRed, 2, "mirrored: left texel lands right");
}

TEST(OverlayShaderWarp, OpacityBlendsTowardTheBackground) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);
    StandaloneOverlayPass pass(d3d);

    ChromaKeyParams none;
    const OverlayPixelConstants pc = MakeOverlayPixelConstants(none, /*mirror=*/false, /*force_opaque=*/true,
                                                               /*opacity=*/0.5f, /*hdr_linear=*/false, 1.0f);
    const std::vector<Bgra> out = pass.Composite({kWhite}, kBlack, pc);

    ExpectNear(out[0], Bgra{128, 128, 128, 255}, 2, "half-opaque white over black");
}

TEST(OverlayShaderWarp, KeyedSpriteHonoursOpacityOnTheKeptPixels) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);
    StandaloneOverlayPass pass(d3d);

    // The preview draws the PiP in a single pass, so a keyed sprite at 50 % opacity
    // must drop the key colour entirely *and* half-blend what it keeps. Red is far
    // from green in CbCr, so it clears the soft band and keeps full alpha — white
    // would not: at tolerance 0.40 it still lands inside green's softness fringe.
    const OverlayPixelConstants pc = MakeOverlayPixelConstants(GreenKey(), /*mirror=*/false, /*force_opaque=*/true,
                                                               /*opacity=*/0.5f, /*hdr_linear=*/false, 1.0f);
    const std::vector<Bgra> out = pass.Composite({kGreen, kRed}, kBlack, pc);

    ExpectNear(out[0], kBlack, 2, "keyed pixel stays fully transparent at any opacity");
    ExpectNear(out[1], Bgra{0, 0, 128, 255}, 2, "kept pixel is half-blended");
}

TEST(OverlayShaderWarp, CursorModePreservesSourceAlpha) {
    auto d3d = CreateWarpDevice();
    ASSERT_TRUE(d3d.device);
    StandaloneOverlayPass pass(d3d);

    ChromaKeyParams none; // disabled + not force_opaque == OverlayMode::Cursor
    const OverlayPixelConstants pc = MakeOverlayPixelConstants(none, /*mirror=*/false, /*force_opaque=*/false,
                                                               /*opacity=*/1.0f, /*hdr_linear=*/false, 1.0f);
    const Bgra transparent{255, 255, 255, 0};
    const std::vector<Bgra> out = pass.Composite({transparent, kWhite}, kRed, pc);

    ExpectNear(out[0], kRed, 2, "alpha 0 sprite texel leaves the background alone");
    ExpectNear(out[1], kWhite, 2, "alpha 255 sprite texel covers the background");
}

} // namespace
} // namespace recorder_core
