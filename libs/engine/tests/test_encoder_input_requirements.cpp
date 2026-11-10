#include <exosnap/engine/codec_types.h>
#include <exosnap/engine/encoder_input_requirements.h>

#include "fakes/fake_video_encoder.h"
#include "nvenc_input_requirements.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace exosnap::engine;

int device_identity;
int other_device_identity;

EncoderInputRequirements Requirements() {
    EncoderInputRequirements input;
    input.storage = EncoderInputStorage::D3D11Texture2D;
    input.format = EncoderInputFormat::Nv12;
    input.codec = VideoCodec::H264;
    input.bit_depth = BitDepth::Bit8;
    input.chroma = ChromaSubsampling::Cs420;
    input.allocation = {1920, 1080};
    input.visible = {0, 0, 1920, 1080};
    input.alignment = {2, 2};
    input.required_bind = EncoderInputBind::RenderTarget;
    input.allowed_bind = EncoderInputBind::RenderTarget | EncoderInputBind::ShaderResource;
    input.pool = {8, 8, 8};
    input.device = &device_identity;
    return input;
}

EncoderInputSurfaceDesc Surface() {
    EncoderInputSurfaceDesc surface;
    surface.storage = EncoderInputStorage::D3D11Texture2D;
    surface.format = EncoderInputFormat::Nv12;
    surface.allocation = {1920, 1080};
    surface.bind = EncoderInputBind::RenderTarget;
    surface.device = &device_identity;
    return surface;
}

TEST(EncoderInputRequirements, CurrentNvencLayoutsPreserveFormatAndExactConfiguredPool) {
    struct Case {
        VideoCodec codec;
        BitDepth depth;
        ChromaSubsampling chroma;
        EncoderInputFormat format;
        EncoderInputBind bind;
    };
    const Case cases[] = {
        {VideoCodec::H264, BitDepth::Bit8, ChromaSubsampling::Cs420, EncoderInputFormat::Nv12,
         EncoderInputBind::RenderTarget},
        {VideoCodec::Hevc, BitDepth::Bit10, ChromaSubsampling::Cs420, EncoderInputFormat::P010,
         EncoderInputBind::RenderTarget},
        {VideoCodec::Av1, BitDepth::Bit10, ChromaSubsampling::Cs420, EncoderInputFormat::P010,
         EncoderInputBind::RenderTarget},
        {VideoCodec::H264, BitDepth::Bit8, ChromaSubsampling::Cs444, EncoderInputFormat::Ayuv,
         EncoderInputBind::UnorderedAccess},
        {VideoCodec::Hevc, BitDepth::Bit8, ChromaSubsampling::Cs444, EncoderInputFormat::Ayuv,
         EncoderInputBind::UnorderedAccess},
    };
    for (const auto& test : cases) {
        for (const uint32_t slot_count : {8u, 12u, 36u}) {
            const auto input = MakeNvencInputRequirements(test.codec, test.depth, test.chroma, {1920, 1080}, slot_count,
                                                          &device_identity);
            EXPECT_TRUE(ValidateEncoderInputForHost(input, test.codec, test.depth, test.chroma, {1920, 1080},
                                                    slot_count, &device_identity));
            EXPECT_EQ(input.format, test.format);
            EXPECT_EQ(input.required_bind, test.bind);
            EXPECT_EQ(input.allocation.width, 1920u);
            EXPECT_EQ(input.allocation.height, 1080u);
            EXPECT_EQ(input.visible.x, 0u);
            EXPECT_EQ(input.visible.y, 0u);
            EXPECT_EQ(input.visible.width, 1920u);
            EXPECT_EQ(input.visible.height, 1080u);
            EXPECT_EQ(input.pool.minimum, slot_count);
            EXPECT_EQ(input.pool.preferred, slot_count);
            EXPECT_EQ(input.pool.maximum, slot_count);
        }
    }
}

TEST(EncoderInputRequirements, UnsupportedNvencColorCombinationsDoNotDefaultToNv12) {
    for (const auto& input : {
             MakeNvencInputRequirements(VideoCodec::H264, BitDepth::Bit10, ChromaSubsampling::Cs420, {1920, 1080}, 8,
                                        &device_identity),
             MakeNvencInputRequirements(VideoCodec::Av1, BitDepth::Bit8, ChromaSubsampling::Cs444, {1920, 1080}, 8,
                                        &device_identity),
             MakeNvencInputRequirements(VideoCodec::Hevc, BitDepth::Bit10, ChromaSubsampling::Cs444, {1920, 1080}, 8,
                                        &device_identity),
         }) {
        EXPECT_EQ(ValidateEncoderInputRequirements(input).code, EncoderInputError::UnsupportedFormat);
    }
}

TEST(EncoderInputRequirements, MalformedContractsFailBeforeAllocation) {
    struct Case {
        const char* name;
        std::function<void(EncoderInputRequirements&)> change;
        EncoderInputError error;
    };
    const Case cases[] = {
        {"unknown storage", [](auto& r) { r.storage = EncoderInputStorage::Unknown; },
         EncoderInputError::UnsupportedStorage},
        {"unknown format", [](auto& r) { r.format = static_cast<EncoderInputFormat>(999); },
         EncoderInputError::UnsupportedFormat},
        {"wrong depth", [](auto& r) { r.bit_depth = BitDepth::Bit10; }, EncoderInputError::ColorMismatch},
        {"wrong chroma", [](auto& r) { r.chroma = ChromaSubsampling::Cs444; }, EncoderInputError::ColorMismatch},
        {"unknown codec", [](auto& r) { r.codec = static_cast<VideoCodec>(999); },
         EncoderInputError::ConfigurationMismatch},
        {"zero width", [](auto& r) { r.allocation.width = 0; }, EncoderInputError::InvalidGeometry},
        {"overflowing width", [](auto& r) { r.allocation.width = std::numeric_limits<uint32_t>::max(); },
         EncoderInputError::InvalidGeometry},
        {"overflowing height", [](auto& r) { r.allocation.height = std::numeric_limits<uint32_t>::max(); },
         EncoderInputError::InvalidGeometry},
        {"zero alignment", [](auto& r) { r.alignment.height = 0; }, EncoderInputError::InvalidAlignment},
        {"unaligned width", [](auto& r) { r.allocation.width = 1919; }, EncoderInputError::InvalidAlignment},
        {"zero visible size", [](auto& r) { r.visible.height = 0; }, EncoderInputError::InvalidGeometry},
        {"visible crop overflow", [](auto& r) { r.visible.x = std::numeric_limits<uint32_t>::max(); },
         EncoderInputError::InvalidGeometry},
        {"visible outside allocation", [](auto& r) { r.visible.y = 2; }, EncoderInputError::InvalidGeometry},
        {"empty pool", [](auto& r) { r.pool.minimum = 0; }, EncoderInputError::InvalidPool},
        {"reversed pool", [](auto& r) { r.pool.maximum = 4; }, EncoderInputError::InvalidPool},
        {"excessive pool", [](auto& r) { r.pool.maximum = 129; }, EncoderInputError::InvalidPool},
        {"preferred outside pool", [](auto& r) { r.pool.preferred = 9; }, EncoderInputError::InvalidPool},
        {"missing device", [](auto& r) { r.device = nullptr; }, EncoderInputError::WrongDevice},
        {"unknown binds", [](auto& r) { r.allowed_bind = static_cast<EncoderInputBind>(0x80000000); },
         EncoderInputError::IncompatibleBindings},
        {"forbidden required bind", [](auto& r) { r.allowed_bind = EncoderInputBind::ShaderResource; },
         EncoderInputError::IncompatibleBindings},
        {"invalid subresource", [](auto& r) { r.subresource = 1; }, EncoderInputError::UnsupportedResourceLayout},
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        auto input = Requirements();
        test.change(input);
        const auto result = ValidateEncoderInputRequirements(input);
        EXPECT_EQ(result.code, test.error);
        EXPECT_NE(result.message[0], '\0');
    }
}

TEST(EncoderInputRequirements, HostRejectsValidLayoutsItsConvertersCannotProduce) {
    for (const auto& change : {
             std::function<void(EncoderInputRequirements&)>{[](auto& r) { r.allocation.height = 1088; }},
             std::function<void(EncoderInputRequirements&)>{[](auto& r) { r.visible = {2, 0, 1918, 1080}; }},
             std::function<void(EncoderInputRequirements&)>{[](auto& r) { r.array_size = 2; }},
             std::function<void(EncoderInputRequirements&)>{[](auto& r) { r.mip_levels = 2; }},
             std::function<void(EncoderInputRequirements&)>{[](auto& r) { r.misc_flags = 1; }},
         }) {
        auto input = Requirements();
        change(input);
        ASSERT_TRUE(ValidateEncoderInputRequirements(input));
        EXPECT_FALSE(ValidateEncoderInputForHost(input, input.codec, input.bit_depth, input.chroma,
                                                 {input.visible.width, input.visible.height}, 8, &device_identity));
    }
}

TEST(EncoderInputRequirements, HostChecksRequestedConfigurationAndActualPool) {
    const auto input = Requirements();
    EXPECT_EQ(ValidateEncoderInputForHost(input, VideoCodec::Hevc, BitDepth::Bit8, ChromaSubsampling::Cs420,
                                          {1920, 1080}, 8, &device_identity)
                  .code,
              EncoderInputError::ConfigurationMismatch);
    EXPECT_EQ(ValidateEncoderInputForHost(input, VideoCodec::H264, BitDepth::Bit10, ChromaSubsampling::Cs420,
                                          {1920, 1080}, 8, &device_identity)
                  .code,
              EncoderInputError::ConfigurationMismatch);
    EXPECT_EQ(ValidateEncoderInputForHost(input, VideoCodec::H264, BitDepth::Bit8, ChromaSubsampling::Cs444,
                                          {1920, 1080}, 8, &device_identity)
                  .code,
              EncoderInputError::ConfigurationMismatch);
    EXPECT_EQ(ValidateEncoderInputForHost(input, VideoCodec::H264, BitDepth::Bit8, ChromaSubsampling::Cs420,
                                          {1280, 720}, 8, &device_identity)
                  .code,
              EncoderInputError::ConfigurationMismatch);
    for (const int32_t slot_count : {-1, 0, 7, 9, 129})
        EXPECT_EQ(ValidateEncoderInputForHost(input, input.codec, input.bit_depth, input.chroma, {1920, 1080},
                                              slot_count, &device_identity)
                      .code,
                  EncoderInputError::InvalidPool);
    EXPECT_EQ(ValidateEncoderInputForHost(input, input.codec, input.bit_depth, input.chroma, {1920, 1080}, 8,
                                          &other_device_identity)
                  .code,
              EncoderInputError::WrongDevice);
}

TEST(EncoderInputRequirements, RegistrationChecksTheActualResourceAgainstTheDeclaredContract) {
    const auto input = Requirements();
    EXPECT_TRUE(ValidateEncoderInputSurface(input, Surface()));
    auto readable = Surface();
    readable.bind = readable.bind | EncoderInputBind::ShaderResource;
    EXPECT_TRUE(ValidateEncoderInputSurface(input, readable));
    struct Case {
        std::function<void(EncoderInputSurfaceDesc&)> change;
        EncoderInputError error;
    };
    const Case cases[] = {
        {[](auto& s) { s.storage = EncoderInputStorage::Unknown; }, EncoderInputError::UnsupportedStorage},
        {[](auto& s) { s.format = EncoderInputFormat::P010; }, EncoderInputError::UnsupportedFormat},
        {[](auto& s) { s.allocation.width = 1280; }, EncoderInputError::InvalidGeometry},
        {[](auto& s) { s.allocation.height = 1088; }, EncoderInputError::InvalidGeometry},
        {[](auto& s) { s.device = &other_device_identity; }, EncoderInputError::WrongDevice},
        {[](auto& s) { s.bind = EncoderInputBind::ShaderResource; }, EncoderInputError::IncompatibleBindings},
        {[](auto& s) { s.bind = s.bind | EncoderInputBind::UnorderedAccess; }, EncoderInputError::IncompatibleBindings},
        {[](auto& s) { s.array_size = 2; }, EncoderInputError::UnsupportedResourceLayout},
        {[](auto& s) { s.mip_levels = 2; }, EncoderInputError::UnsupportedResourceLayout},
        {[](auto& s) { s.subresource = 1; }, EncoderInputError::UnsupportedResourceLayout},
        {[](auto& s) { s.sample_count = 2; }, EncoderInputError::UnsupportedResourceLayout},
        {[](auto& s) { s.sample_quality = 1; }, EncoderInputError::UnsupportedResourceLayout},
        {[](auto& s) { s.misc_flags = 1; }, EncoderInputError::UnsupportedResourceLayout},
        {[](auto& s) { s.gpu_default_usage = false; }, EncoderInputError::UnsupportedResourceLayout},
        {[](auto& s) { s.cpu_access_flags = 1; }, EncoderInputError::UnsupportedResourceLayout},
    };
    for (size_t index = 0; index < std::size(cases); ++index) {
        SCOPED_TRACE(index);
        auto surface = Surface();
        cases[index].change(surface);
        EXPECT_EQ(ValidateEncoderInputSurface(input, surface).code, cases[index].error);
    }
}

TEST(EncoderInputRequirements, ConfiguredBackendSuppliesTheAllocationAndRetiresItOnDestroy) {
    testutil::FakeVideoEncoder encoder(3);
    EXPECT_FALSE(ValidateEncoderInputRequirements(encoder.GetInputRequirements()));
    encoder.SetCodec(VideoCodec::Hevc);
    encoder.SetBitDepth(BitDepth::Bit10);
    std::string error;
    ASSERT_TRUE(encoder.Open(&device_identity, error));
    ASSERT_TRUE(encoder.Configure(1280, 720, 60, 1, error));
    const auto input = encoder.GetInputRequirements();
    EXPECT_TRUE(ValidateEncoderInputForHost(input, VideoCodec::Hevc, BitDepth::Bit10, ChromaSubsampling::Cs420,
                                            {1280, 720}, encoder.SlotCount(), &device_identity));
    EXPECT_EQ(input.format, EncoderInputFormat::P010);
    EXPECT_EQ(input.pool.minimum, 3u);
    EXPECT_EQ(input.pool.maximum, 3u);
    encoder.Destroy();
    EXPECT_FALSE(ValidateEncoderInputRequirements(encoder.GetInputRequirements()));
}

} // namespace
