#include "models/EditPointerGeometry.h"

#include <array>
#include <gtest/gtest.h>
#include <limits>

namespace exosnap::edit {

TEST(EditRangePointerGeometry, TrueEndpointsUseNearestHitAndLastActiveTieWithoutHiddenBody) {
    EXPECT_EQ(ResolveRangePointerTarget(1, 0, 8), RangePointerTarget::Start);
    EXPECT_EQ(ResolveRangePointerTarget(7, 0, 8), RangePointerTarget::End);
    EXPECT_EQ(ResolveRangePointerTarget(4, 0, 8, 6, true, RangePointerTarget::End), RangePointerTarget::End);
    EXPECT_EQ(ResolveRangePointerTarget(4, 0, 8), RangePointerTarget::Start);
    EXPECT_EQ(ResolveRangePointerTarget(50, 0, 100), RangePointerTarget::Body);
    EXPECT_EQ(ResolveRangePointerTarget(120, 0, 100), RangePointerTarget::Create);
    EXPECT_EQ(ResolveRangePointerTarget(-2, -4, 100), RangePointerTarget::Start);
}

TEST(EditRangePointerGeometry, InactiveRangeCreatesAndInvalidGeometryNeverCaptures) {
    EXPECT_EQ(ResolveRangePointerTarget(50, 0, 100, 6, false), RangePointerTarget::Create);
    EXPECT_EQ(ResolveRangePointerTarget(50, 100, 0), RangePointerTarget::None);
    EXPECT_EQ(ResolveRangePointerTarget(std::numeric_limits<double>::quiet_NaN(), 0, 100), RangePointerTarget::None);
}

TEST(EditRangePointerGeometry, AutoscrollUsesBoundedPenetrationAndNearestOverlappingEdge) {
    EXPECT_DOUBLE_EQ(EdgeAutoScrollVelocity(50, 100), 0);
    EXPECT_DOUBLE_EQ(EdgeAutoScrollVelocity(12, 100), -300);
    EXPECT_DOUBLE_EQ(EdgeAutoScrollVelocity(88, 100), 300);
    EXPECT_DOUBLE_EQ(EdgeAutoScrollVelocity(-100, 100), -600);
    EXPECT_DOUBLE_EQ(EdgeAutoScrollVelocity(200, 100), 600);
    EXPECT_DOUBLE_EQ(EdgeAutoScrollVelocity(10, 20), 0);
    EXPECT_LT(EdgeAutoScrollVelocity(9, 20), 0);
    EXPECT_GT(EdgeAutoScrollVelocity(11, 20), 0);
    EXPECT_DOUBLE_EQ(EdgeAutoScrollVelocity(1, 0), 0);
}

TEST(EditRangePointerGeometry, TrackTargetCrossesAdjacentMidpointsWithHysteresisAndSupportsJumps) {
    const std::array rows{TrackRowBounds{0, 64}, TrackRowBounds{64, 108}, TrackRowBounds{108, 172}};
    EXPECT_EQ(ResolveDragTrack(0, 89, rows), 0);
    EXPECT_EQ(ResolveDragTrack(0, 90, rows), 1);
    EXPECT_EQ(ResolveDragTrack(1, 82, rows), 1);
    EXPECT_EQ(ResolveDragTrack(1, 27, rows), 0);
    EXPECT_EQ(ResolveDragTrack(0, 200, rows), 2);
    EXPECT_EQ(ResolveDragTrack(2, -10, rows), 0);
    EXPECT_EQ(ResolveDragTrack(-1, 80, rows), -1);
    const std::array invalid{TrackRowBounds{0, 64}, TrackRowBounds{50, 100}};
    EXPECT_EQ(ResolveDragTrack(0, 80, invalid), -1);
}

TEST(EditPointerGeometry, EligibleCornersPrecedeTrimAndOnlyTheirOwnJunctionEndIsSuppressed) {
    ClipPointerGeometry clip{100, 64, 18};
    EXPECT_EQ(ResolveClipPointerTarget(clip, 2, 2), ClipPointerTarget::FadeIn);
    EXPECT_EQ(ResolveClipPointerTarget(clip, 98, 2), ClipPointerTarget::FadeOut);
    EXPECT_EQ(ResolveClipPointerTarget(clip, 2, 25), ClipPointerTarget::TrimStart);
    EXPECT_EQ(ResolveClipPointerTarget(clip, 98, 25), ClipPointerTarget::TrimEnd);
    clip.fade_out_available = false;
    EXPECT_EQ(ResolveClipPointerTarget(clip, 98, 2), ClipPointerTarget::TrimEnd);
    EXPECT_EQ(ResolveClipPointerTarget(clip, 2, 2), ClipPointerTarget::FadeIn);
}

TEST(EditPointerGeometry, NarrowAndShortRowsHaveExactHeaderFallbacks) {
    ClipPointerGeometry clip{23.9, 112, 18, true};
    EXPECT_EQ(ResolveClipPointerTarget(clip, 1, 50), ClipPointerTarget::Header);
    clip.width = 24;
    EXPECT_EQ(ResolveClipPointerTarget(clip, 3.9, 50), ClipPointerTarget::TrimStart);
    EXPECT_EQ(ResolveClipPointerTarget(clip, 4, 50), ClipPointerTarget::Content);
    clip.height = 43;
    clip.header_height = 43;
    EXPECT_EQ(ResolveClipPointerTarget(clip, 12, 30), ClipPointerTarget::Header);
    EXPECT_EQ(ResolveClipPointerTarget(clip, 1, 30), ClipPointerTarget::TrimStart);
    clip.width = 40;
    clip.height = 44;
    clip.header_height = 16;
    EXPECT_EQ(ResolveClipPointerTarget(clip, 13.9, 13.9), ClipPointerTarget::FadeIn);
    EXPECT_EQ(ResolveClipPointerTarget(clip, 14, 13.9), ClipPointerTarget::Header);
}

TEST(EditPointerGeometry, JunctionPrecedesGainAndGainUsesSharedPiecewiseDecibelGeometry) {
    ClipPointerGeometry clip{200, 118, 18, true};
    EXPECT_DOUBLE_EQ(ClipGainLineY(clip), 48);
    EXPECT_EQ(ResolveClipPointerTarget(clip, 100, 48), ClipPointerTarget::Gain);
    EXPECT_EQ(ResolveClipPointerTarget(clip, 100, 51.01), ClipPointerTarget::Content);
    clip.gain_db = 24;
    EXPECT_DOUBLE_EQ(ClipGainLineY(clip), 18);
    clip.gain_db = -60;
    EXPECT_DOUBLE_EQ(ClipGainLineY(clip), 118);
    clip.junction_content = true;
    EXPECT_EQ(ResolveClipPointerTarget(clip, 100, 50), ClipPointerTarget::Junction);
    EXPECT_EQ(ResolveClipPointerTarget(clip, 100, 12), ClipPointerTarget::Header);
    EXPECT_EQ(ResolveClipPointerTarget(clip, -1, 12), ClipPointerTarget::None);
    EXPECT_EQ(ResolveClipPointerTarget(clip, std::numeric_limits<double>::quiet_NaN(), 12), ClipPointerTarget::None);
}

TEST(EditPointerGeometry, CapturedTypeKeepsItsOwnThresholdAxis) {
    EXPECT_FALSE(ClipPointerThresholdReached(ClipPointerTarget::TrimStart, 2, 100, 10));
    EXPECT_TRUE(ClipPointerThresholdReached(ClipPointerTarget::TrimStart, 10, 0, 10));
    EXPECT_FALSE(ClipPointerThresholdReached(ClipPointerTarget::Gain, 100, 2, 10));
    EXPECT_TRUE(ClipPointerThresholdReached(ClipPointerTarget::Gain, 0, -10, 10));
    EXPECT_TRUE(ClipPointerThresholdReached(ClipPointerTarget::Header, 6, 8, 10));
    EXPECT_TRUE(ClipPointerThresholdReached(ClipPointerTarget::Content, 6, 8, 10));
    EXPECT_FALSE(ClipPointerThresholdReached(ClipPointerTarget::Junction, 100, 100, 10));
}

} // namespace exosnap::edit
