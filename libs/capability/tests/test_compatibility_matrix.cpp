#include <gtest/gtest.h>

#include <capability/capability_builder.h>
#include <capability/capability_set.h>
#include <capability/config_types.h>

#include <cstddef>

namespace exosnap::capability {
namespace {

TEST(CapabilityMatrixTest, AllEnumTuplesAreQueryable) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    size_t queried = 0;

    for (const Container c : AllContainers()) {
        for (const VideoCodec v : AllVideoCodecs()) {
            for (const AudioCodec a : AllAudioCodecs()) {
                for (const ChromaSubsampling cs : AllChromaModes()) {
                    for (const BitDepth bd : AllBitDepths()) {
                        const SupportAnnotation annotation = caps.QueryCombo(c, v, a, cs, bd);
                        (void)annotation;
                        ++queried;
                    }
                }
            }
        }
    }

    EXPECT_EQ(queried, 162u);
}

TEST(CapabilityMatrixTest, MatrixRequiredPairsMatchBaseline) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    EXPECT_EQ(caps.QueryCombo(Container::Matroska, VideoCodec::Av1Nvenc, AudioCodec::AacMf, ChromaSubsampling::Cs420,
                              BitDepth::Bit8)
                  .level,
              SupportLevel::Available);

    EXPECT_EQ(caps.QueryCombo(Container::Matroska, VideoCodec::Av1Nvenc, AudioCodec::Opus, ChromaSubsampling::Cs420,
                              BitDepth::Bit8)
                  .level,
              SupportLevel::Available);

    EXPECT_EQ(caps.QueryCombo(Container::WebM, VideoCodec::Av1Nvenc, AudioCodec::Opus, ChromaSubsampling::Cs420,
                              BitDepth::Bit8)
                  .level,
              SupportLevel::Available);

    EXPECT_EQ(caps.QueryCombo(Container::WebM, VideoCodec::Av1Nvenc, AudioCodec::AacMf, ChromaSubsampling::Cs420,
                              BitDepth::Bit8)
                  .level,
              SupportLevel::Invalid);

    EXPECT_EQ(caps.QueryCombo(Container::Mp4, VideoCodec::Av1Nvenc, AudioCodec::Opus, ChromaSubsampling::Cs420,
                              BitDepth::Bit8)
                  .level,
              SupportLevel::Invalid);

    EXPECT_EQ(caps.QueryCombo(Container::WebM, VideoCodec::HevcNvenc, AudioCodec::Opus, ChromaSubsampling::Cs420,
                              BitDepth::Bit8)
                  .level,
              SupportLevel::Invalid);
}

TEST(CapabilityMatrixTest, ChromaAndBitDepthUnsupportedPathsAreNotImplemented) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    EXPECT_EQ(caps.QueryCombo(Container::Matroska, VideoCodec::Av1Nvenc, AudioCodec::AacMf, ChromaSubsampling::Cs420,
                              BitDepth::Bit10)
                  .level,
              SupportLevel::NotImplemented);

    EXPECT_EQ(caps.QueryCombo(Container::Matroska, VideoCodec::Av1Nvenc, AudioCodec::AacMf, ChromaSubsampling::Cs444,
                              BitDepth::Bit8)
                  .level,
              SupportLevel::NotImplemented);
}

} // namespace
} // namespace exosnap::capability
