#include <gtest/gtest.h>

#include <capability/capability_builder.h>
#include <capability/capability_set.h>
#include <capability/config_types.h>
#include <capability/container_compat_registry.h>
#include <capability/support_level.h>

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

    EXPECT_EQ(
        caps.QueryCombo(Container::Matroska, VideoCodec::Av1, AudioCodec::Aac, ChromaSubsampling::Cs420, BitDepth::Bit8)
            .level,
        SupportLevel::Available);

    EXPECT_EQ(caps.QueryCombo(Container::Matroska, VideoCodec::Av1, AudioCodec::Opus, ChromaSubsampling::Cs420,
                              BitDepth::Bit8)
                  .level,
              SupportLevel::Available);

    // MKV + H.264 + AAC: now available (default profile)
    EXPECT_EQ(caps.QueryCombo(Container::Matroska, VideoCodec::H264, AudioCodec::Aac, ChromaSubsampling::Cs420,
                              BitDepth::Bit8)
                  .level,
              SupportLevel::Available);

    EXPECT_EQ(
        caps.QueryCombo(Container::WebM, VideoCodec::Av1, AudioCodec::Opus, ChromaSubsampling::Cs420, BitDepth::Bit8)
            .level,
        SupportLevel::Available);

    EXPECT_EQ(
        caps.QueryCombo(Container::WebM, VideoCodec::Av1, AudioCodec::Aac, ChromaSubsampling::Cs420, BitDepth::Bit8)
            .level,
        SupportLevel::Invalid);

    EXPECT_EQ(
        caps.QueryCombo(Container::Mp4, VideoCodec::Av1, AudioCodec::Opus, ChromaSubsampling::Cs420, BitDepth::Bit8)
            .level,
        SupportLevel::Invalid);

    EXPECT_EQ(
        caps.QueryCombo(Container::WebM, VideoCodec::Hevc, AudioCodec::Opus, ChromaSubsampling::Cs420, BitDepth::Bit8)
            .level,
        SupportLevel::Invalid);

    // MP4 + H.264 + AAC: available
    EXPECT_EQ(
        caps.QueryCombo(Container::Mp4, VideoCodec::H264, AudioCodec::Aac, ChromaSubsampling::Cs420, BitDepth::Bit8)
            .level,
        SupportLevel::Available);
}

TEST(CapabilityMatrixTest, MP4_H264_AAC_IsAvailable) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    EXPECT_EQ(
        caps.QueryCombo(Container::Mp4, VideoCodec::H264, AudioCodec::Aac, ChromaSubsampling::Cs420, BitDepth::Bit8)
            .level,
        SupportLevel::Available);
}

TEST(CapabilityMatrixTest, MP4_UnsupportedCombos_AreNotImplementedOrInvalid) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    // MP4 + AV1 + AAC: deferred
    EXPECT_EQ(
        caps.QueryCombo(Container::Mp4, VideoCodec::Av1, AudioCodec::Aac, ChromaSubsampling::Cs420, BitDepth::Bit8)
            .level,
        SupportLevel::NotImplemented);

    // MP4 + HEVC + AAC: 0.7.0 hvc1-in-MP4 path — registry Allowed → ValidUnvalidated
    // (selectable with caveat; Apple/NLE + GPU verification pending).
    EXPECT_EQ(
        caps.QueryCombo(Container::Mp4, VideoCodec::Hevc, AudioCodec::Aac, ChromaSubsampling::Cs420, BitDepth::Bit8)
            .level,
        SupportLevel::ValidUnvalidated);

    // MP4 + H.264 + Opus: invalid (Opus not valid for MP4)
    EXPECT_EQ(
        caps.QueryCombo(Container::Mp4, VideoCodec::H264, AudioCodec::Opus, ChromaSubsampling::Cs420, BitDepth::Bit8)
            .level,
        SupportLevel::Invalid);

    // WebM + AAC: invalid
    EXPECT_EQ(
        caps.QueryCombo(Container::WebM, VideoCodec::Av1, AudioCodec::Aac, ChromaSubsampling::Cs420, BitDepth::Bit8)
            .level,
        SupportLevel::Invalid);
}

TEST(CapabilityMatrixTest, ChromaAndBitDepthUnsupportedPathsAreNotImplemented) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    // 4:4:4 is never available for AV1 (NVENC AV1 is 4:2:0 only).
    EXPECT_EQ(
        caps.QueryCombo(Container::Matroska, VideoCodec::Av1, AudioCodec::Aac, ChromaSubsampling::Cs444, BitDepth::Bit8)
            .level,
        SupportLevel::NotImplemented);

    // 4:2:2 remains unimplemented for every codec (Ada NVENC has no 4:2:2).
    EXPECT_EQ(caps.QueryCombo(Container::Matroska, VideoCodec::Hevc, AudioCodec::Aac, ChromaSubsampling::Cs422,
                              BitDepth::Bit8)
                  .level,
              SupportLevel::NotImplemented);

    // 10-bit with H.264 is not implemented (H.264 is 8-bit only).
    EXPECT_EQ(caps.QueryCombo(Container::Matroska, VideoCodec::H264, AudioCodec::Aac, ChromaSubsampling::Cs420,
                              BitDepth::Bit10)
                  .level,
              SupportLevel::NotImplemented);
}

TEST(CapabilityMatrixTest, FourFourFourIsPerCodecEightBitOnly) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    // 4:4:4 8-bit is a real (not-yet-hardware-validated) path for H.264 and HEVC.
    EXPECT_EQ(
        caps.QueryCombo(Container::Mp4, VideoCodec::H264, AudioCodec::Aac, ChromaSubsampling::Cs444, BitDepth::Bit8)
            .level,
        SupportLevel::ValidUnvalidated);
    EXPECT_EQ(caps.QueryCombo(Container::Matroska, VideoCodec::Hevc, AudioCodec::Aac, ChromaSubsampling::Cs444,
                              BitDepth::Bit8)
                  .level,
              SupportLevel::ValidUnvalidated);
    EXPECT_TRUE(IsSelectable(caps.QueryChroma444(VideoCodec::H264)));
    EXPECT_TRUE(IsSelectable(caps.QueryChroma444(VideoCodec::Hevc)));
    EXPECT_FALSE(IsSelectable(caps.QueryChroma444(VideoCodec::Av1)));

    // 4:4:4 + 10-bit is out of scope — not selectable for any codec.
    EXPECT_EQ(caps.QueryCombo(Container::Matroska, VideoCodec::Hevc, AudioCodec::Aac, ChromaSubsampling::Cs444,
                              BitDepth::Bit10)
                  .level,
              SupportLevel::NotImplemented);
}

TEST(CapabilityMatrixTest, TenBitHevcAndAv1AreValidUnvalidated) {
    // 0.7.0 S5: 10-bit (HEVC Main10 / AV1 10-bit, P010) is implemented but not yet
    // hardware-validated → ValidUnvalidated (selectable with a warning).
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    EXPECT_EQ(caps.QueryCombo(Container::Matroska, VideoCodec::Hevc, AudioCodec::Aac, ChromaSubsampling::Cs420,
                              BitDepth::Bit10)
                  .level,
              SupportLevel::ValidUnvalidated);

    EXPECT_EQ(caps.QueryCombo(Container::Matroska, VideoCodec::Av1, AudioCodec::Aac, ChromaSubsampling::Cs420,
                              BitDepth::Bit10)
                  .level,
              SupportLevel::ValidUnvalidated);

    EXPECT_EQ(
        caps.QueryCombo(Container::WebM, VideoCodec::Av1, AudioCodec::Opus, ChromaSubsampling::Cs420, BitDepth::Bit10)
            .level,
        SupportLevel::ValidUnvalidated);
}

// ---------------------------------------------------------------------------
// Registry level <-> user-selectability
//
// One gate, one answer. Documentation, the container matrix in the product spec
// and the UI all describe what a user can actually pick, and this is the only
// place that decides it: a combination is offered exactly when its registry
// level is Recommended or Allowed. Experimental means technically muxable and
// deliberately NOT offered -- the distinction that a support table has to keep.
// ---------------------------------------------------------------------------

TEST(CapabilityMatrixTest, OnlyRecommendedAndAllowedAreSelectable) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    for (const Container c : AllContainers()) {
        for (const VideoCodec v : AllVideoCodecs()) {
            for (const AudioCodec a : AllAudioCodecs()) {
                const ContainerCompatLevel level = ContainerCompatRegistry::Query(c, v, a).level;
                const bool registry_offers =
                    level == ContainerCompatLevel::Recommended || level == ContainerCompatLevel::Allowed;
                const SupportAnnotation annotation = caps.QueryCombo(c, v, a, ChromaSubsampling::Cs420, BitDepth::Bit8);

                EXPECT_EQ(IsSelectable(annotation.level), registry_offers)
                    << "container=" << static_cast<int>(c) << " video=" << static_cast<int>(v)
                    << " audio=" << static_cast<int>(a) << " level=" << ToString(level);
            }
        }
    }
}

TEST(CapabilityMatrixTest, Mp4Av1IsNotUserSelectable) {
    // The claim the product spec's container matrix has to match: AV1-in-MP4 is
    // Experimental in the registry and therefore never offered in 0.9.
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    for (const AudioCodec a : AllAudioCodecs()) {
        EXPECT_FALSE(IsSelectable(
            caps.QueryCombo(Container::Mp4, VideoCodec::Av1, a, ChromaSubsampling::Cs420, BitDepth::Bit8).level))
            << "audio=" << static_cast<int>(a);
    }

    // The two MP4 video codecs that ARE offered, so the test above cannot pass by
    // rejecting all of MP4.
    EXPECT_TRUE(
        caps.QueryCombo(Container::Mp4, VideoCodec::H264, AudioCodec::Aac, ChromaSubsampling::Cs420, BitDepth::Bit8)
            .level == SupportLevel::Available);
    EXPECT_TRUE(IsSelectable(
        caps.QueryCombo(Container::Mp4, VideoCodec::Hevc, AudioCodec::Aac, ChromaSubsampling::Cs420, BitDepth::Bit8)
            .level));
}

} // namespace
} // namespace exosnap::capability
