#include "diagnostics/PresentModeMapping.h"
#include <gtest/gtest.h>

using namespace exosnap::diagnostics;

TEST(PresentModeMapping, InvalidEventIsUnavailable) {
    RawPresentEvent ev{};
    ev.valid = false;
    const PresentSample s = MapPresentEvent(ev);
    EXPECT_FALSE(s.available);
    EXPECT_EQ(s.mode, PresentMode::Unknown);
}

TEST(PresentModeMapping, ComposedFlipMapsToComposed) {
    RawPresentEvent ev{/*present_mode_code=*/4, /*sync_interval=*/1,
                       /*tearing_flag=*/false, /*interval_ms=*/16.6, /*valid=*/true};
    const PresentSample s = MapPresentEvent(ev);
    EXPECT_TRUE(s.available);
    EXPECT_EQ(s.mode, PresentMode::Composed);
    EXPECT_FALSE(s.tearing);
    EXPECT_DOUBLE_EQ(s.present_interval_ms, 16.6);
}

TEST(PresentModeMapping, IndependentFlipMapsThrough) {
    RawPresentEvent ev{3, 1, false, 8.3, true}; // Hardware_Independent_Flip
    EXPECT_EQ(MapPresentEvent(ev).mode, PresentMode::IndependentFlip);
}

TEST(PresentModeMapping, HardwareComposedIndependentFlipIsIndependentFlip) {
    RawPresentEvent ev{8, 1, false, 8.3, true}; // Hardware_Composed_Independent_Flip
    EXPECT_EQ(MapPresentEvent(ev).mode, PresentMode::IndependentFlip);
}

TEST(PresentModeMapping, LegacyFlipMapsToExclusiveFullscreen) {
    RawPresentEvent ev{1, 0, true, 6.9, true};
    const PresentSample s = MapPresentEvent(ev);
    EXPECT_EQ(s.mode, PresentMode::ExclusiveFullscreen);
    EXPECT_TRUE(s.tearing); // sync_interval 0 + tearing flag
}

TEST(PresentModeMapping, SyncIntervalZeroImpliesTearing) {
    RawPresentEvent ev{3, /*sync_interval=*/0, /*tearing_flag=*/false, 7.0, true};
    EXPECT_TRUE(MapPresentEvent(ev).tearing); // interval 0 = uncapped/tearing-capable
}
