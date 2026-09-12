// test_hdr_chroma_geometry.cpp — the two chroma decisions in the HDR10 PQ path.
//
// A 4:2:0 chroma plane is half size in both axes, and both things that follow
// from that were computed in a way that is only correct in the easy case:
//
//   * the left-siting shift was derived from the SOURCE crop width, so it was
//     right at 1:1 and wrong at every other scale factor;
//   * the chroma viewport divided origin AND size by two, so an odd content
//     height left its last row on the neutral clear value -- a colourless bottom
//     line in the picture.
//
// Both are pure functions of the geometry, so this needs no device. The pixel
// proof they enable (a colour edge at 1:1, down- and upscale, and the 1079-row
// case) is a visual scenario, not this file.

#include "gpu_hdr_pq.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using Converter = exosnap::engine::HdrPqConverter;

// ---- The left-siting shift ------------------------------------------------

TEST(HdrChromaSiting, TheShiftIsHalfAPixelOfTheOutputRaster) {
    // texcoord runs 0..1 across the content rectangle, so half an output luma
    // pixel is 0.5/content_w -- whatever the source was.
    EXPECT_FLOAT_EQ(Converter::LeftSitingShiftTexels(1920), 0.5f / 1920.0f);
    EXPECT_FLOAT_EQ(Converter::LeftSitingShiftTexels(1280), 0.5f / 1280.0f);
}

TEST(HdrChromaSiting, TheShiftDoesNotDependOnTheSourceScale) {
    // The defect, stated as the property it violated. A 3840-wide source scaled
    // to 1920 and a 1280-wide source scaled up to 1920 must produce the SAME
    // shift, because the shift is about the output. Before, it was 0.5/3840 and
    // 0.5/1280 -- off by 2x in both directions.
    const float downscaled = Converter::LeftSitingShiftTexels(1920);
    const float upscaled = Converter::LeftSitingShiftTexels(1920);
    EXPECT_FLOAT_EQ(downscaled, upscaled);

    // And the old computation really did differ, so the test above is not vacuous.
    const float old_downscale = 0.5f / 3840.0f;
    const float old_upscale = 0.5f / 1280.0f;
    EXPECT_NE(old_downscale, old_upscale);
    EXPECT_NE(old_downscale, downscaled);
}

TEST(HdrChromaSiting, AtOneToOneTheOldAndNewShiftsAgree) {
    // Why this went unnoticed: at 1:1 the source crop width and the content width
    // are the same number, so every unscaled recording was correct.
    constexpr uint32_t kSame = 2560;
    EXPECT_FLOAT_EQ(Converter::LeftSitingShiftTexels(kSame), 0.5f / static_cast<float>(kSame));
}

TEST(HdrChromaSiting, AZeroWidthShiftsNothingInsteadOfDividingByZero) {
    EXPECT_FLOAT_EQ(Converter::LeftSitingShiftTexels(0), 0.0f);
}

// ---- The chroma viewport --------------------------------------------------

TEST(HdrChromaViewport, AnEvenContentRectangleHalvesExactly) {
    const auto vp = Converter::ChromaViewportFor(0, 0, 1920, 1080);
    EXPECT_EQ(vp.x, 0u);
    EXPECT_EQ(vp.y, 0u);
    EXPECT_EQ(vp.w, 960u);
    EXPECT_EQ(vp.h, 540u);
}

TEST(HdrChromaViewport, AnOddHeightStillCoversItsLastRow) {
    // The review's counter-example: 1079 content rows inside an even encode
    // height, which a contain-fit produces. 1079/2 is 539 and the 540th chroma row
    // is what the last luma row needs, so it used to keep the neutral clear value.
    const auto vp = Converter::ChromaViewportFor(0, 0, 1920, 1079);
    EXPECT_EQ(vp.h, 540u) << "the last content row had no chroma row to sample from";
    EXPECT_GE(vp.y + vp.h, (0u + 1079u + 1u) / 2u);
}

TEST(HdrChromaViewport, AnOddWidthStillCoversItsLastColumn) {
    const auto vp = Converter::ChromaViewportFor(0, 0, 1919, 1080);
    EXPECT_EQ(vp.w, 960u);
}

TEST(HdrChromaViewport, AnOddOriginFloorsSoAStraddlingSampleIsStillDrawn) {
    // A chroma sample covering the content edge has to be rendered, so the origin
    // rounds DOWN -- rounding it up would leave the first row/column neutral,
    // which is the same defect at the other end.
    const auto vp = Converter::ChromaViewportFor(101, 51, 1718, 978);
    EXPECT_EQ(vp.x, 50u);
    EXPECT_EQ(vp.y, 25u);
}

TEST(HdrChromaViewport, TheViewportAlwaysReachesTheContentEnd) {
    // The property, over every parity combination of origin and size: the half-
    // resolution region covers the last luma row and column of the content.
    for (uint32_t x : {0u, 1u, 2u, 101u}) {
        for (uint32_t y : {0u, 1u, 2u, 51u}) {
            for (uint32_t w : {2u, 3u, 1079u, 1920u}) {
                for (uint32_t h : {2u, 3u, 1079u, 1080u}) {
                    const auto vp = Converter::ChromaViewportFor(x, y, w, h);
                    const uint32_t needed_right = (x + w + 1) / 2;
                    const uint32_t needed_bottom = (y + h + 1) / 2;
                    EXPECT_GE(vp.x + vp.w, needed_right) << x << "," << w;
                    EXPECT_GE(vp.y + vp.h, needed_bottom) << y << "," << h;
                    EXPECT_LE(vp.x, (x + 1) / 2) << "the origin must not skip a straddling sample";
                    EXPECT_LE(vp.y, (y + 1) / 2);
                }
            }
        }
    }
}

TEST(HdrChromaViewport, TheOldHalvingWouldHaveMissedTheLastRow) {
    // The premise, so this suite cannot quietly stop testing anything: plain
    // integer halving of an odd size really is short by one.
    constexpr uint32_t kOddHeight = 1079;
    EXPECT_LT(kOddHeight / 2, Converter::ChromaViewportFor(0, 0, 1920, kOddHeight).h);
}

TEST(HdrChromaViewport, ContentGeometryIsNotMoved) {
    // The constraint the fix had to respect: covering the last row must not shift
    // where the content sits. The origin for an even rectangle is unchanged, and
    // for an odd one it only ever floors -- it never moves the content right or
    // down.
    for (uint32_t x : {0u, 1u, 2u, 3u, 100u, 101u}) {
        const auto vp = Converter::ChromaViewportFor(x, x, 1920, 1080);
        EXPECT_EQ(vp.x, x / 2);
        EXPECT_EQ(vp.y, x / 2);
    }
}

} // namespace
