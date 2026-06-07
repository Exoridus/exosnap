#include <gtest/gtest.h>

#include "../services/PreviewHelpers.h"

#include <Windows.h>

namespace exosnap {
namespace {

// ---------------------------------------------------------------------------
// PreviewCropBox::IsValid
// ---------------------------------------------------------------------------

TEST(PreviewCropBox, IsValidWithPositiveOffsetsAndDimensions) {
    PreviewCropBox box{100, 200, 800, 600};
    EXPECT_TRUE(box.IsValid());
}

TEST(PreviewCropBox, IsValidAtZeroOrigin) {
    PreviewCropBox box{0, 0, 1920, 1080};
    EXPECT_TRUE(box.IsValid());
}

TEST(PreviewCropBox, IsInvalidWithNegativeX) {
    PreviewCropBox box{-1, 0, 800, 600};
    EXPECT_FALSE(box.IsValid());
}

TEST(PreviewCropBox, IsInvalidWithNegativeY) {
    PreviewCropBox box{0, -1, 800, 600};
    EXPECT_FALSE(box.IsValid());
}

TEST(PreviewCropBox, IsInvalidWithZeroWidth) {
    PreviewCropBox box{0, 0, 0, 600};
    EXPECT_FALSE(box.IsValid());
}

TEST(PreviewCropBox, IsInvalidWithZeroHeight) {
    PreviewCropBox box{0, 0, 800, 0};
    EXPECT_FALSE(box.IsValid());
}

TEST(PreviewCropBox, IsInvalidWithNegativeWidth) {
    PreviewCropBox box{0, 0, -100, 600};
    EXPECT_FALSE(box.IsValid());
}

TEST(PreviewCropBox, IsInvalidWithNegativeHeight) {
    PreviewCropBox box{0, 0, 800, -1};
    EXPECT_FALSE(box.IsValid());
}

TEST(PreviewCropBox, DefaultConstructedIsInvalid) {
    PreviewCropBox box{};
    EXPECT_FALSE(box.IsValid());
}

// ---------------------------------------------------------------------------
// RegionToCropBox — coordinate conversion
// ---------------------------------------------------------------------------

// Primary monitor at virtual-screen origin (0, 0): monitor-relative offset equals
// the virtual-screen coordinate.
TEST(RegionToCropBox, PrimaryMonitorZeroOriginPreservesCoordinates) {
    const PreviewCropBox box = RegionToCropBox(100, 200, 800, 600, 0, 0);
    EXPECT_EQ(box.x, 100);
    EXPECT_EQ(box.y, 200);
    EXPECT_EQ(box.width, 800);
    EXPECT_EQ(box.height, 600);
    EXPECT_TRUE(box.IsValid());
}

// Secondary monitor to the right of primary: subtract the monitor's virtual-screen
// origin to obtain the offset within the WGC frame.
TEST(RegionToCropBox, SecondaryMonitorPositiveOriginSubtracted) {
    // Region at virtual screen (2020, 200); monitor starts at (1920, 0).
    const PreviewCropBox box = RegionToCropBox(2020, 200, 800, 600, 1920, 0);
    // Monitor-relative: x = 2020 - 1920 = 100
    EXPECT_EQ(box.x, 100);
    EXPECT_EQ(box.y, 200);
    EXPECT_EQ(box.width, 800);
    EXPECT_EQ(box.height, 600);
    EXPECT_TRUE(box.IsValid());
}

// Secondary monitor to the left with a negative virtual-screen origin.
TEST(RegionToCropBox, SecondaryMonitorNegativeOriginSubtracted) {
    // Region at virtual screen (-1820, 200); monitor starts at (-1920, 0).
    const PreviewCropBox box = RegionToCropBox(-1820, 200, 800, 600, -1920, 0);
    // Monitor-relative: x = -1820 - (-1920) = 100
    EXPECT_EQ(box.x, 100);
    EXPECT_EQ(box.y, 200);
    EXPECT_EQ(box.width, 800);
    EXPECT_EQ(box.height, 600);
    EXPECT_TRUE(box.IsValid());
}

// Region dimensions (width/height) pass through unchanged regardless of origin.
TEST(RegionToCropBox, DimensionsPassThroughUnchanged) {
    const PreviewCropBox box = RegionToCropBox(1920, 540, 1280, 720, 1920, 0);
    EXPECT_EQ(box.width, 1280);
    EXPECT_EQ(box.height, 720);
}

// A region that starts before the monitor origin produces a negative x or y.
// RecordPage must reject this (box.IsValid() == false).
TEST(RegionToCropBox, RegionBeforeMonitorOriginGivesNegativeOffset) {
    // Region at (0, 0) but monitor starts at (1920, 0) — region is left of the monitor.
    const PreviewCropBox box = RegionToCropBox(0, 0, 800, 600, 1920, 0);
    EXPECT_LT(box.x, 0);
    EXPECT_FALSE(box.IsValid());
}

// Region top edge before monitor top edge (vertically stacked arrangement).
TEST(RegionToCropBox, RegionAboveMonitorOriginGivesNegativeYOffset) {
    const PreviewCropBox box = RegionToCropBox(0, 100, 800, 600, 0, 200);
    EXPECT_LT(box.y, 0);
    EXPECT_FALSE(box.IsValid());
}

// Monitor at zero offset: the resulting x and y equal the virtual-screen coords.
TEST(RegionToCropBox, ZeroMonitorOriginIsIdentity) {
    const PreviewCropBox box = RegionToCropBox(500, 300, 640, 480, 0, 0);
    EXPECT_EQ(box.x, 500);
    EXPECT_EQ(box.y, 300);
}

// ---------------------------------------------------------------------------
// ComputeContainFitRect — crop dimensions drive contain-fit (aspect preservation)
// ---------------------------------------------------------------------------

// When the cropped source matches the backbuffer exactly, it fills the backbuffer.
TEST(CropBoxContainFit, ExactMatchFillsBackbuffer) {
    LONG x = -1, y = -1, w = -1, h = -1;
    ComputeContainFitRect(800, 600, 800, 600, x, y, w, h);
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);
    EXPECT_EQ(w, 800);
    EXPECT_EQ(h, 600);
}

// A 16:9 crop inside a 4:3 backbuffer is letterboxed (top/bottom bars).
TEST(CropBoxContainFit, WideRegionCropLetterboxedIn4x3Backbuffer) {
    // Crop: 1920×1080 (16:9)  Backbuffer: 800×600 (4:3)
    LONG x = -1, y = -1, w = -1, h = -1;
    ComputeContainFitRect(800, 600, 1920, 1080, x, y, w, h);
    // Width fills (800), height must be letterboxed (< 600, > 0)
    EXPECT_EQ(x, 0);
    EXPECT_GT(y, 0);
    EXPECT_EQ(w, 800);
    EXPECT_LT(h, 600);
    // Aspect ratio preserved: w/h ≈ 16/9
    EXPECT_NEAR(static_cast<double>(w) / h, 16.0 / 9.0, 0.02);
}

// A 9:16 crop (vertical) inside a 16:9 backbuffer is pillarboxed.
TEST(CropBoxContainFit, VerticalRegionCropPillarboxedIn16x9Backbuffer) {
    // Crop: 1080×1920 (9:16)  Backbuffer: 1920×1080 (16:9)
    LONG x = -1, y = -1, w = -1, h = -1;
    ComputeContainFitRect(1920, 1080, 1080, 1920, x, y, w, h);
    EXPECT_GT(x, 0);
    EXPECT_EQ(y, 0);
    EXPECT_LT(w, 1920);
    EXPECT_EQ(h, 1080);
    // Aspect ratio preserved: w/h ≈ 9/16
    EXPECT_NEAR(static_cast<double>(w) / h, 9.0 / 16.0, 0.02);
}

// A square 1:1 crop inside a 16:9 backbuffer is pillarboxed.
TEST(CropBoxContainFit, SquareRegionCropPillarboxedIn16x9Backbuffer) {
    LONG x = -1, y = -1, w = -1, h = -1;
    ComputeContainFitRect(1920, 1080, 1080, 1080, x, y, w, h);
    EXPECT_GT(x, 0);
    EXPECT_EQ(y, 0);
    EXPECT_EQ(w, h); // square aspect preserved
}

// A 4:5 portrait crop (e.g. Instagram format) in a 16:9 backbuffer is pillarboxed.
TEST(CropBoxContainFit, PortraitRegionCropPreservesAspectIn16x9Backbuffer) {
    LONG x = -1, y = -1, w = -1, h = -1;
    ComputeContainFitRect(1920, 1080, 1080, 1350, x, y, w, h);
    EXPECT_GT(x, 0);
    EXPECT_EQ(y, 0);
    EXPECT_NEAR(static_cast<double>(w) / h, 4.0 / 5.0, 0.02);
}

// ---------------------------------------------------------------------------
// Preview / recording parity
// ---------------------------------------------------------------------------

// The recording engine receives the CaptureRegion in virtual-screen coordinates;
// the preview applies RegionToCropBox with the monitor origin to get the same
// rectangle in monitor-relative space.  Round-trip verification:
//   virtual-screen region ─ monitor origin = monitor-relative offset.
TEST(PreviewRecordingParity, VirtualScreenRegionMatchesMonitorRelativeCropAfterConversion) {
    // Virtual-screen coords (e.g., a region on the primary monitor at origin 0,0)
    constexpr int32_t region_x = 200;
    constexpr int32_t region_y = 150;
    constexpr int32_t region_w = 1280;
    constexpr int32_t region_h = 720;
    constexpr int32_t monitor_origin_x = 0;
    constexpr int32_t monitor_origin_y = 0;

    // What the recording engine uses directly:
    // cfg.crop_region = {region_x, region_y, region_w, region_h}
    // (virtual-screen, same as CaptureRegion)

    // What the preview renders:
    const PreviewCropBox box =
        RegionToCropBox(region_x, region_y, region_w, region_h, monitor_origin_x, monitor_origin_y);

    // On the primary monitor (origin 0,0) the two representations are identical.
    EXPECT_EQ(box.x, region_x);
    EXPECT_EQ(box.y, region_y);
    EXPECT_EQ(box.width, region_w);
    EXPECT_EQ(box.height, region_h);
}

TEST(PreviewRecordingParity, SecondaryMonitorPreviewCropMatchesRecordingRegionDimensions) {
    // Recording stores the full virtual-screen region (used as-is by the engine).
    constexpr int32_t region_x = 2100;
    constexpr int32_t region_y = 50;
    constexpr int32_t region_w = 800;
    constexpr int32_t region_h = 600;
    constexpr int32_t monitor_origin_x = 1920;
    constexpr int32_t monitor_origin_y = 0;

    const PreviewCropBox box =
        RegionToCropBox(region_x, region_y, region_w, region_h, monitor_origin_x, monitor_origin_y);

    // Preview offset is monitor-relative; dimensions are always the same.
    EXPECT_EQ(box.x, region_x - monitor_origin_x); // 180
    EXPECT_EQ(box.y, region_y - monitor_origin_y); // 50
    EXPECT_EQ(box.width, region_w);                // unchanged
    EXPECT_EQ(box.height, region_h);               // unchanged
}

} // namespace
} // namespace exosnap
