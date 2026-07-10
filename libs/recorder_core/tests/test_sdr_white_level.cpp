#include <gtest/gtest.h>

#include <recorder_core/sdr_white_level.h>

#include <cmath>

using recorder_core::EffectiveOverlayReferenceWhiteNits;
using recorder_core::SdrWhiteLevelRawToNits;

TEST(SdrWhiteLevelTest, RawToNitsUsesEightyNitsPerThousand) {
    EXPECT_FLOAT_EQ(SdrWhiteLevelRawToNits(1000), 80.0f);   // scRGB 1.0
    EXPECT_FLOAT_EQ(SdrWhiteLevelRawToNits(2537), 202.96f); // ~203 (typical HDR default)
    EXPECT_FLOAT_EQ(SdrWhiteLevelRawToNits(3000), 240.0f);  // common Windows default
    EXPECT_FLOAT_EQ(SdrWhiteLevelRawToNits(0), 0.0f);
}

TEST(SdrWhiteLevelTest, EffectiveRefWhitePassesPlausibleValues) {
    EXPECT_FLOAT_EQ(EffectiveOverlayReferenceWhiteNits(80.0f), 80.0f);
    EXPECT_FLOAT_EQ(EffectiveOverlayReferenceWhiteNits(240.0f), 240.0f);
    EXPECT_FLOAT_EQ(EffectiveOverlayReferenceWhiteNits(480.0f), 480.0f);
}

TEST(SdrWhiteLevelTest, EffectiveRefWhiteFallsBackTo203) {
    EXPECT_FLOAT_EQ(EffectiveOverlayReferenceWhiteNits(0.0f), 203.0f);    // unknown/query failed
    EXPECT_FLOAT_EQ(EffectiveOverlayReferenceWhiteNits(79.9f), 203.0f);   // below slider floor
    EXPECT_FLOAT_EQ(EffectiveOverlayReferenceWhiteNits(5000.0f), 203.0f); // above slider ceiling
    EXPECT_FLOAT_EQ(EffectiveOverlayReferenceWhiteNits(std::nanf("")), 203.0f);
    EXPECT_FLOAT_EQ(EffectiveOverlayReferenceWhiteNits(-1.0f), 203.0f);
}
