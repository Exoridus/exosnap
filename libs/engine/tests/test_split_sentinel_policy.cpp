// Pure unit tests for ShouldEmitSplitSentinel (split_sentinel_policy.h).
// No GPU/NVENC session, no video thread.

#include "split_sentinel_policy.h"

#include <gtest/gtest.h>

using namespace exosnap::engine;

namespace {

TEST(ShouldEmitSplitSentinel, NotArmed_NeverEmits) {
    // Even an exact keyframe/PTS match must not emit when no split is armed.
    EXPECT_FALSE(ShouldEmitSplitSentinel(/*armed=*/false, /*forced_pts=*/100, /*keyframe=*/true, /*pkt_pts=*/100));
}

TEST(ShouldEmitSplitSentinel, ArmedButNotKeyframe_DoesNotEmit) {
    EXPECT_FALSE(ShouldEmitSplitSentinel(/*armed=*/true, /*forced_pts=*/100, /*keyframe=*/false, /*pkt_pts=*/100));
}

TEST(ShouldEmitSplitSentinel, ArmedKeyframeAtExactForcedPts_Emits) {
    // Sync-mode-equivalent case: the routed packet IS the just-submitted
    // forced frame.
    EXPECT_TRUE(ShouldEmitSplitSentinel(/*armed=*/true, /*forced_pts=*/100, /*keyframe=*/true, /*pkt_pts=*/100));
}

TEST(ShouldEmitSplitSentinel, ArmedKeyframeBeforeForcedPts_DoesNotEmit) {
    // The async-submit-ahead hazard: an EARLIER, already-in-flight natural
    // GOP keyframe must not absorb the split.
    EXPECT_FALSE(ShouldEmitSplitSentinel(/*armed=*/true, /*forced_pts=*/100, /*keyframe=*/true, /*pkt_pts=*/99));
}

TEST(ShouldEmitSplitSentinel, ArmedKeyframeAfterForcedPts_StillEmits) {
    // Defensive >=: if the exact forced-PTS packet is somehow never observed
    // as a keyframe, the next keyframe after it still carries the sentinel
    // rather than losing it.
    EXPECT_TRUE(ShouldEmitSplitSentinel(/*armed=*/true, /*forced_pts=*/100, /*keyframe=*/true, /*pkt_pts=*/101));
}

} // namespace
