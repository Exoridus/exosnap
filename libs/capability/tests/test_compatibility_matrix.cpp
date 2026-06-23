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

    // 3 containers × 3 video × 4 audio (Opus/AAC/PCM/FLAC) × 3 chroma × 2 depth.
    EXPECT_EQ(queried, 216u);
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

    // MKV + H.264 + AAC: now available (default profile)
    EXPECT_EQ(caps.QueryCombo(Container::Matroska, VideoCodec::H264Nvenc, AudioCodec::AacMf, ChromaSubsampling::Cs420,
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

    // MP4 + H.264 + AAC: available
    EXPECT_EQ(caps.QueryCombo(Container::Mp4, VideoCodec::H264Nvenc, AudioCodec::AacMf, ChromaSubsampling::Cs420,
                              BitDepth::Bit8)
                  .level,
              SupportLevel::Available);
}

TEST(CapabilityMatrixTest, MP4_H264_AAC_IsAvailable) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    EXPECT_EQ(caps.QueryCombo(Container::Mp4, VideoCodec::H264Nvenc, AudioCodec::AacMf, ChromaSubsampling::Cs420,
                              BitDepth::Bit8)
                  .level,
              SupportLevel::Available);
}

TEST(CapabilityMatrixTest, MP4_UnsupportedCombos_AreNotImplementedOrInvalid) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    // MP4 + AV1 + AAC: deferred
    EXPECT_EQ(caps.QueryCombo(Container::Mp4, VideoCodec::Av1Nvenc, AudioCodec::AacMf, ChromaSubsampling::Cs420,
                              BitDepth::Bit8)
                  .level,
              SupportLevel::NotImplemented);

    // MP4 + HEVC + AAC: 0.7.0 hvc1-in-MP4 path — registry Allowed → ValidUnvalidated
    // (selectable with caveat; Apple/NLE + GPU verification pending).
    EXPECT_EQ(caps.QueryCombo(Container::Mp4, VideoCodec::HevcNvenc, AudioCodec::AacMf, ChromaSubsampling::Cs420,
                              BitDepth::Bit8)
                  .level,
              SupportLevel::ValidUnvalidated);

    // MP4 + H.264 + Opus: invalid (Opus not valid for MP4)
    EXPECT_EQ(caps.QueryCombo(Container::Mp4, VideoCodec::H264Nvenc, AudioCodec::Opus, ChromaSubsampling::Cs420,
                              BitDepth::Bit8)
                  .level,
              SupportLevel::Invalid);

    // WebM + AAC: invalid
    EXPECT_EQ(caps.QueryCombo(Container::WebM, VideoCodec::Av1Nvenc, AudioCodec::AacMf, ChromaSubsampling::Cs420,
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
