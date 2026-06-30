#include <gtest/gtest.h>

#include <capability/capability_set.h>
#include <capability/codec_selection.h>
#include <capability/config_types.h>
#include <capability/support_level.h>

namespace exosnap::capability {
namespace {

// Helper: set a video codec dimension annotation.
void SetCodec(CapabilitySet& caps, VideoCodec v, SupportLevel level) {
    caps.video_codecs[v] = SupportAnnotation{level, ""};
}

// (MKV, all 3 Available) -> AV1 (preference order picks AV1 first).
TEST(CodecSelectionTest, MkvAllAvailable_PicksAv1) {
    CapabilitySet caps;
    SetCodec(caps, VideoCodec::Av1Nvenc, SupportLevel::Available);
    SetCodec(caps, VideoCodec::HevcNvenc, SupportLevel::Available);
    SetCodec(caps, VideoCodec::H264Nvenc, SupportLevel::Available);

    const auto best = BestAvailableVideoCodec(caps, Container::Matroska);
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(*best, VideoCodec::Av1Nvenc);
}

// (MKV, AV1=NotImplemented, HEVC/H264 Available) -> HEVC (next in order).
TEST(CodecSelectionTest, MkvAv1Missing_PicksHevc) {
    CapabilitySet caps;
    SetCodec(caps, VideoCodec::Av1Nvenc, SupportLevel::NotImplemented);
    SetCodec(caps, VideoCodec::HevcNvenc, SupportLevel::Available);
    SetCodec(caps, VideoCodec::H264Nvenc, SupportLevel::Available);

    const auto best = BestAvailableVideoCodec(caps, Container::Matroska);
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(*best, VideoCodec::HevcNvenc);
}

// ValidUnvalidated also qualifies as GPU-supported.
TEST(CodecSelectionTest, MkvAv1ValidUnvalidated_StillPicksAv1) {
    CapabilitySet caps;
    SetCodec(caps, VideoCodec::Av1Nvenc, SupportLevel::ValidUnvalidated);
    SetCodec(caps, VideoCodec::HevcNvenc, SupportLevel::Available);

    const auto best = BestAvailableVideoCodec(caps, Container::Matroska);
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(*best, VideoCodec::Av1Nvenc);
}

// (WebM, AV1 Available) -> AV1 (WebM only carries AV1; AV1 is valid there).
TEST(CodecSelectionTest, WebmAv1Available_PicksAv1) {
    CapabilitySet caps;
    SetCodec(caps, VideoCodec::Av1Nvenc, SupportLevel::Available);
    SetCodec(caps, VideoCodec::HevcNvenc, SupportLevel::Available);
    SetCodec(caps, VideoCodec::H264Nvenc, SupportLevel::Available);

    const auto best = BestAvailableVideoCodec(caps, Container::WebM);
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(*best, VideoCodec::Av1Nvenc);
}

// (WebM, AV1=NotImplemented) -> nullopt: HEVC/H264 are container-prohibited in
// WebM even though the GPU might support them, so nothing qualifies.
TEST(CodecSelectionTest, WebmAv1Missing_HevcAndH264Prohibited_ReturnsNullopt) {
    CapabilitySet caps;
    SetCodec(caps, VideoCodec::Av1Nvenc, SupportLevel::NotImplemented);
    SetCodec(caps, VideoCodec::HevcNvenc, SupportLevel::Available);
    SetCodec(caps, VideoCodec::H264Nvenc, SupportLevel::Available);

    const auto best = BestAvailableVideoCodec(caps, Container::WebM);
    EXPECT_FALSE(best.has_value());
}

// (MP4, only H264 Available) -> H264. AV1/HEVC absent from the map are Invalid
// (not selectable), so H.264 is the best valid MP4 codec.
TEST(CodecSelectionTest, Mp4OnlyH264Available_PicksH264) {
    CapabilitySet caps;
    SetCodec(caps, VideoCodec::H264Nvenc, SupportLevel::Available);
    // AV1 and HEVC deliberately not present -> Invalid -> not selectable.

    const auto best = BestAvailableVideoCodec(caps, Container::Mp4);
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(*best, VideoCodec::H264Nvenc);
}

// Empty caps -> nothing GPU-supported -> nullopt.
TEST(CodecSelectionTest, EmptyCaps_ReturnsNullopt) {
    CapabilitySet caps;
    EXPECT_FALSE(BestAvailableVideoCodec(caps, Container::Matroska).has_value());
}

// Canonical visible labels (single-source spelling canon).
TEST(CodecSelectionTest, VisibleLabels_AreCanonical) {
    EXPECT_EQ(VisibleVideoCodecLabel(VideoCodec::Av1Nvenc), "AV1");
    EXPECT_EQ(VisibleVideoCodecLabel(VideoCodec::HevcNvenc), "HEVC");
    EXPECT_EQ(VisibleVideoCodecLabel(VideoCodec::H264Nvenc), "H.264");
}

} // namespace
} // namespace exosnap::capability
