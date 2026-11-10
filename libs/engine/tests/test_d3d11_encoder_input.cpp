#include "d3d11_encoder_input.h"
#include "nvenc_input_requirements.h"

#include <exosnap/engine/codec_types.h>
#include <exosnap/engine/encoder_input_requirements.h>
#include <gtest/gtest.h>

#include <winrt/base.h>

namespace {

using namespace exosnap::engine;

TEST(D3D11EncoderInput, NativeAllocationUsesTheDeclaredFormatAndBindings) {
    int device;
    struct Case {
        BitDepth depth;
        ChromaSubsampling chroma;
        DXGI_FORMAT format;
        UINT bind;
    };
    for (const auto test :
         {Case{BitDepth::Bit8, ChromaSubsampling::Cs420, DXGI_FORMAT_NV12, D3D11_BIND_RENDER_TARGET},
          Case{BitDepth::Bit10, ChromaSubsampling::Cs420, DXGI_FORMAT_P010, D3D11_BIND_RENDER_TARGET},
          Case{BitDepth::Bit8, ChromaSubsampling::Cs444, DXGI_FORMAT_AYUV, D3D11_BIND_UNORDERED_ACCESS}}) {
        const auto input =
            MakeNvencInputRequirements(VideoCodec::Hevc, test.depth, test.chroma, {1920, 1080}, 8, &device);
        const auto desc = D3D11EncoderInputTextureDesc(input);
        EXPECT_EQ(desc.Format, test.format);
        EXPECT_EQ(desc.BindFlags, test.bind);
        EXPECT_EQ(desc.Width, 1920u);
        EXPECT_EQ(desc.Height, 1080u);
        EXPECT_EQ(desc.ArraySize, 1u);
        EXPECT_EQ(desc.MipLevels, 1u);
        EXPECT_EQ(desc.SampleDesc.Count, 1u);
        EXPECT_EQ(desc.SampleDesc.Quality, 0u);
        EXPECT_EQ(desc.CPUAccessFlags, 0u);
        EXPECT_EQ(desc.MiscFlags, 0u);
        EXPECT_EQ(desc.Usage, D3D11_USAGE_DEFAULT);
    }
    EXPECT_EQ(D3D11EncoderInputFormat(EncoderInputFormat::Unknown), DXGI_FORMAT_UNKNOWN);
    EXPECT_EQ(EncoderInputFormatFromD3D11(DXGI_FORMAT_B8G8R8A8_UNORM), EncoderInputFormat::Unknown);
}

TEST(D3D11EncoderInput, RegistrationInspectsTheTextureAndDeviceBeforeAnyEncoderCall) {
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                                            device.put(), nullptr, context.put())));
    auto input = MakeNvencInputRequirements(VideoCodec::H264, BitDepth::Bit8, ChromaSubsampling::Cs420, {64, 64}, 8,
                                            device.get());
    EXPECT_EQ(ValidateD3D11EncoderInputTexture(input, nullptr).code, EncoderInputError::UnsupportedResourceLayout);

    // An ordinary BGRA resource is a valid D3D11 texture but cannot be
    // registered as the declared NV12 input, even on the correct device.
    auto desc = D3D11EncoderInputTextureDesc(input);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    winrt::com_ptr<ID3D11Texture2D> wrong_format;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, wrong_format.put())));
    EXPECT_EQ(ValidateD3D11EncoderInputTexture(input, wrong_format.get()).code, EncoderInputError::UnsupportedFormat);

    winrt::com_ptr<ID3D11Device> other_device;
    ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                                            other_device.put(), nullptr, nullptr)));
    input.device = other_device.get();
    EXPECT_EQ(ValidateD3D11EncoderInputTexture(input, wrong_format.get()).code, EncoderInputError::WrongDevice);
}

} // namespace
