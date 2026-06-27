#include "recorder_core/frame_pacing.h"
#include <gtest/gtest.h>
using namespace recorder_core;

TEST(PacingRingSize, AdaptiveFromRatio) {
    EXPECT_EQ(ComputePacingRingSize(144, 60), 5u); // ceil(144/60)=3, +2 = 5
    EXPECT_EQ(ComputePacingRingSize(240, 60), 6u); // ceil(240/60)=4, +2 = 6
    EXPECT_EQ(ComputePacingRingSize(60, 60), 4u);  // ceil=1, +2=3 -> clamped up to min 4
}
TEST(PacingRingSize, ClampsToMax) {
    EXPECT_EQ(ComputePacingRingSize(1000, 60), 12u); // huge ratio clamps to 12
}
TEST(PacingRingSize, UnknownRefreshFallsBackTo8) {
    EXPECT_EQ(ComputePacingRingSize(0, 60), 8u);
    EXPECT_EQ(ComputePacingRingSize(144, 0), 8u);
}
