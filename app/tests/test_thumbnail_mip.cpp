// Which mip a tile is read back from. Getting this wrong is silent: one level
// too far and the tile is a magnified blur, one level too few and the readback
// costs what the mip chain was built to avoid.

#include <gtest/gtest.h>

#include "services/ThumbnailMip.h"

namespace {

using exosnap::ChooseMipLevel;
using exosnap::MipExtent;

// A full chain over 3840x2160 has 12 levels (down to 1x1).
constexpr uint32_t kChain4k = 12;

TEST(ThumbnailMip, PicksTheSmallestLevelStillLargerThanTheTile) {
    // 3840 >> 3 == 480, 2160 >> 3 == 270. One further and the width falls to 240,
    // below the 256 the tile wants.
    EXPECT_EQ(ChooseMipLevel(3840, 2160, kChain4k, 256, 144), 3u);
    EXPECT_EQ(MipExtent(3840, 3), 480u);
    EXPECT_EQ(MipExtent(2160, 3), 270u);
}

TEST(ThumbnailMip, NeverReturnsALevelSmallerThanTheTileInEitherAxis) {
    for (uint32_t w : {800u, 1920u, 2560u, 3840u, 5120u}) {
        for (uint32_t h : {600u, 1080u, 1440u, 2160u}) {
            const uint32_t level = ChooseMipLevel(w, h, kChain4k, 256, 144);
            if (w < 256 || h < 144) {
                EXPECT_EQ(level, 0u);
                continue;
            }
            EXPECT_GE(MipExtent(w, level), 256u) << w << "x" << h;
            EXPECT_GE(MipExtent(h, level), 144u) << w << "x" << h;
        }
    }
}

TEST(ThumbnailMip, ASourceNoLargerThanTheTileStaysAtLevelZero) {
    EXPECT_EQ(ChooseMipLevel(256, 144, kChain4k, 256, 144), 0u);
    EXPECT_EQ(ChooseMipLevel(200, 100, kChain4k, 256, 144), 0u);
}

// The shorter axis binds. A wide, flat window must not be halved past its height.
TEST(ThumbnailMip, TheAxisThatRunsOutFirstDecides) {
    // 3840x200: width would allow level 3 (480), but 200 >> 1 == 100 < 144.
    EXPECT_EQ(ChooseMipLevel(3840, 200, kChain4k, 256, 144), 0u);
}

TEST(ThumbnailMip, AChainOfOneLevelHasNothingToChoose) {
    EXPECT_EQ(ChooseMipLevel(3840, 2160, 1, 256, 144), 0u);
}

TEST(ThumbnailMip, ADegenerateTileSizeFallsBackToTheTopLevel) {
    EXPECT_EQ(ChooseMipLevel(3840, 2160, kChain4k, 0, 144), 0u);
    EXPECT_EQ(ChooseMipLevel(3840, 2160, kChain4k, 256, 0), 0u);
}

TEST(ThumbnailMip, TheLevelStaysInsideTheChain) {
    // A 1x1 tile would like to go all the way down; it may not go past the end.
    EXPECT_LT(ChooseMipLevel(3840, 2160, kChain4k, 1, 1), kChain4k);
}

TEST(ThumbnailMip, MipExtentNeverCollapsesToZero) {
    EXPECT_EQ(MipExtent(1, 5), 1u);
    EXPECT_EQ(MipExtent(2160, 11), 1u);
}

} // namespace
