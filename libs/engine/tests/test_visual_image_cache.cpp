#include <exosnap/engine/gpu_compositor.h>

#include <gtest/gtest.h>

#include <array>

using namespace exosnap::engine;

namespace {

class VisualImageCache : public testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(
            SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                        nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put())));
        ASSERT_TRUE(compositor.Init(device.get(), context.get(), 2, 2, error)) << error;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 2;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        const std::array<uint8_t, 16> black{};
        const D3D11_SUBRESOURCE_DATA initial{black.data(), 8, 0};
        ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, &initial, background.put())));
        frame.width = frame.height = 2;
        frame.generation = 1;
        frame.pixels.assign(16, 255);
    }

    bool Draw(const SourceToken& token, const WebcamFrameSnapshot* pixels, uint64_t resource = 1) {
        if (!compositor.BeginFrame(background.get(), error))
            return false;
        return compositor.DrawVisualImage(token, resource, 1, pixels, {0, 0, 2, 2}, false, {}, error);
    }

    std::array<uint8_t, 4> Pixel() {
        D3D11_TEXTURE2D_DESC desc{};
        compositor.Result()->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::com_ptr<ID3D11Texture2D> staging;
        const auto created = device->CreateTexture2D(&desc, nullptr, staging.put());
        EXPECT_TRUE(SUCCEEDED(created));
        if (FAILED(created))
            return {};
        context->CopyResource(staging.get(), compositor.Result());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto mapped_result = context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped);
        EXPECT_TRUE(SUCCEEDED(mapped_result));
        if (FAILED(mapped_result))
            return {};
        std::array<uint8_t, 4> result{};
        std::copy_n(static_cast<const uint8_t*>(mapped.pData), result.size(), result.begin());
        context->Unmap(staging.get(), 0);
        return result;
    }

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<ID3D11Texture2D> background;
    GpuCompositor compositor;
    WebcamFrameSnapshot frame;
    SourceToken first{{VisualSourceId{{1}}, VisualRuntimeGeneration{1}}, SourceRequestRevision{1}};
    SourceToken second{{VisualSourceId{{2}}, VisualRuntimeGeneration{2}}, SourceRequestRevision{1}};
    std::string error;
};

TEST_F(VisualImageCache, ReusesOnlyTheExactTokenResourceAndSequence) {
    const std::array addresses{first.address};
    ASSERT_TRUE(compositor.PrepareVisualImages(addresses, error));
    ASSERT_TRUE(Draw(first, &frame));
    EXPECT_EQ(compositor.WebcamUploadCount(), 1u);
    ASSERT_TRUE(Draw(first, nullptr));
    EXPECT_EQ(Pixel(), (std::array<uint8_t, 4>{255, 255, 255, 255}));
    EXPECT_EQ(compositor.WebcamUploadCount(), 1u);

    auto next = first;
    ++next.request.value;
    frame.pixels.assign(16, 0);
    ASSERT_TRUE(Draw(next, &frame));
    EXPECT_EQ(compositor.WebcamUploadCount(), 2u);
    EXPECT_EQ(Pixel()[0], 0u);
    ASSERT_TRUE(Draw(next, &frame, 2));
    EXPECT_EQ(compositor.WebcamUploadCount(), 3u);
}

TEST_F(VisualImageCache, PreparationRetainsReorderedSourcesAndEvictsRemovedSources) {
    const std::array addresses{first.address, second.address};
    ASSERT_TRUE(compositor.PrepareVisualImages(addresses, error));
    ASSERT_TRUE(Draw(first, &frame));
    frame.pixels.assign(16, 0);
    ASSERT_TRUE(Draw(second, &frame));
    const std::array reversed{second.address, first.address};
    ASSERT_TRUE(compositor.PrepareVisualImages(reversed, error));
    ASSERT_TRUE(Draw(first, nullptr));
    EXPECT_EQ(Pixel()[0], 255u);
    ASSERT_TRUE(Draw(second, nullptr));
    EXPECT_EQ(Pixel()[0], 0u);
    EXPECT_EQ(compositor.WebcamUploadCount(), 2u);
    ASSERT_TRUE(compositor.PrepareVisualImages({}, error));
    ASSERT_TRUE(compositor.PrepareVisualImages(addresses, error));
    ASSERT_TRUE(Draw(first, nullptr));
    EXPECT_EQ(Pixel()[0], 0u);
}

TEST_F(VisualImageCache, SpriteIdentityPreventsCrossSourceReuseWithoutStickUploads) {
    ASSERT_TRUE(compositor.PrepareVisualSprites(1, error));
    auto white = std::make_shared<SpriteAtlas>();
    white->width = white->height = 1;
    white->generation = 1;
    white->pixels = {255, 255, 255, 255};
    SpriteLayerSnapshot layer{white, {{{0, 0, 1, 1}, {0, 0, 1, 1}, 1}}};
    ASSERT_TRUE(compositor.BeginFrame(background.get(), error));
    ASSERT_TRUE(compositor.DrawVisualSprite(0, first, 1, layer, {0, 0, 2, 2}, error));
    EXPECT_EQ(compositor.SpriteAtlasUploadCount(), 1u);
    layer.quads[0].destination.x = .1F;
    ASSERT_TRUE(compositor.DrawVisualSprite(0, first, 1, layer, {0, 0, 2, 2}, error));
    EXPECT_EQ(compositor.SpriteAtlasUploadCount(), 1u);
    auto black = std::make_shared<SpriteAtlas>(*white);
    black->pixels = {0, 0, 0, 255};
    layer.atlas = black;
    layer.quads[0].destination.x = 0;
    ASSERT_TRUE(compositor.BeginFrame(background.get(), error));
    ASSERT_TRUE(compositor.DrawVisualSprite(0, second, 2, layer, {0, 0, 2, 2}, error));
    EXPECT_EQ(compositor.SpriteAtlasUploadCount(), 2u);
    EXPECT_EQ(Pixel()[0], 0u);
}

} // namespace
