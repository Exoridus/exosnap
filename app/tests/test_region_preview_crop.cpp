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

// ---------------------------------------------------------------------------
// PreviewCropBox::operator==
// ---------------------------------------------------------------------------

TEST(PreviewCropBoxEquality, IdenticalBoxesAreEqual) {
    PreviewCropBox a{100, 200, 800, 600};
    PreviewCropBox b{100, 200, 800, 600};
    EXPECT_TRUE(a == b);
}

TEST(PreviewCropBoxEquality, DifferentXNotEqual) {
    EXPECT_FALSE((PreviewCropBox{0, 0, 800, 600} == PreviewCropBox{1, 0, 800, 600}));
}

TEST(PreviewCropBoxEquality, DifferentYNotEqual) {
    EXPECT_FALSE((PreviewCropBox{0, 0, 800, 600} == PreviewCropBox{0, 1, 800, 600}));
}

TEST(PreviewCropBoxEquality, DifferentWidthNotEqual) {
    EXPECT_FALSE((PreviewCropBox{0, 0, 800, 600} == PreviewCropBox{0, 0, 801, 600}));
}

TEST(PreviewCropBoxEquality, DifferentHeightNotEqual) {
    EXPECT_FALSE((PreviewCropBox{0, 0, 800, 600} == PreviewCropBox{0, 0, 800, 601}));
}

// ---------------------------------------------------------------------------
// PreviewConfigKey — helper factories for tests
// ---------------------------------------------------------------------------
namespace {

// Monitor target (no crop) at the given index/native_id.
PreviewConfigKey MakeDisplayKey(int index, intptr_t native_id = 42) {
    PreviewConfigKey k;
    k.target_index = index;
    k.native_id = native_id;
    k.kind = 0; // Monitor
    k.has_crop = false;
    return k;
}

// Window target (no crop).
PreviewConfigKey MakeWindowKey(int index, intptr_t native_id = 99) {
    PreviewConfigKey k;
    k.target_index = index;
    k.native_id = native_id;
    k.kind = 1; // Window
    k.has_crop = false;
    return k;
}

// Region target: monitor with a crop region in virtual-screen coordinates.
PreviewConfigKey MakeRegionKey(int index, intptr_t native_id, int32_t rx, int32_t ry, int32_t rw, int32_t rh) {
    PreviewConfigKey k;
    k.target_index = index;
    k.native_id = native_id;
    k.kind = 0; // Monitor
    k.has_crop = true;
    k.region_x = rx;
    k.region_y = ry;
    k.region_w = rw;
    k.region_h = rh;
    return k;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// PreviewConfigKey — equality
// ---------------------------------------------------------------------------

TEST(PreviewConfigKey, DefaultIsInvalid) {
    PreviewConfigKey k;
    EXPECT_FALSE(k.IsValid());
    EXPECT_EQ(k.target_index, -1);
}

TEST(PreviewConfigKey, ValidDisplayKeyIsValid) {
    EXPECT_TRUE(MakeDisplayKey(0).IsValid());
}

TEST(PreviewConfigKey, SameDisplayKeysAreEqual) {
    EXPECT_TRUE(MakeDisplayKey(0) == MakeDisplayKey(0));
}

TEST(PreviewConfigKey, DifferentTargetIndexNotEqual) {
    EXPECT_FALSE(MakeDisplayKey(0) == MakeDisplayKey(1));
}

TEST(PreviewConfigKey, DifferentNativeIdNotEqual) {
    EXPECT_FALSE(MakeDisplayKey(0, 10) == MakeDisplayKey(0, 20));
}

TEST(PreviewConfigKey, DisplayAndWindowNotEqual) {
    EXPECT_FALSE(MakeDisplayKey(0) == MakeWindowKey(0, 42));
}

TEST(PreviewConfigKey, SameRegionKeysAreEqual) {
    auto a = MakeRegionKey(0, 42, 100, 200, 800, 600);
    auto b = MakeRegionKey(0, 42, 100, 200, 800, 600);
    EXPECT_TRUE(a == b);
}

TEST(PreviewConfigKey, RegionAndDisplayNotEqualEvenSameTarget) {
    auto region = MakeRegionKey(0, 42, 100, 200, 800, 600);
    auto display = MakeDisplayKey(0, 42);
    EXPECT_FALSE(region == display);
}

// ---------------------------------------------------------------------------
// NeedsPreviewRestart — state-transition tests
//
// Each test corresponds to one of the required transition scenarios from the
// REGION-PREVIEW-CROP-R2 spec.
// ---------------------------------------------------------------------------

// Transition 1: Region crop set → active config has crop.
TEST(NeedsPreviewRestart, RegionCropSetRequiresRestartFromNoPreview) {
    const PreviewConfigKey no_preview{}; // default: target_index == -1
    const PreviewConfigKey region = MakeRegionKey(0, 42, 100, 200, 800, 600);
    EXPECT_TRUE(NeedsPreviewRestart(region, no_preview));
}

// Transition 2: Region → Display → crop must be cleared.
TEST(NeedsPreviewRestart, RegionToDisplayRequiresRestart) {
    const PreviewConfigKey region = MakeRegionKey(0, 42, 100, 200, 800, 600);
    const PreviewConfigKey display = MakeDisplayKey(0, 42);
    // Switching from Region to Display changes has_crop → restart required.
    EXPECT_TRUE(NeedsPreviewRestart(display, region));
}

// Transition 3: Region → Window → crop must be cleared.
TEST(NeedsPreviewRestart, RegionToWindowRequiresRestart) {
    const PreviewConfigKey region = MakeRegionKey(0, 42, 100, 200, 800, 600);
    const PreviewConfigKey window = MakeWindowKey(1, 99);
    EXPECT_TRUE(NeedsPreviewRestart(window, region));
}

// Transition 4: Display → Region → crop must be applied.
TEST(NeedsPreviewRestart, DisplayToRegionRequiresRestart) {
    const PreviewConfigKey display = MakeDisplayKey(0, 42);
    const PreviewConfigKey region = MakeRegionKey(0, 42, 100, 200, 800, 600);
    EXPECT_TRUE(NeedsPreviewRestart(region, display));
}

// Transition 5: Window → Region → crop must be applied.
TEST(NeedsPreviewRestart, WindowToRegionRequiresRestart) {
    const PreviewConfigKey window = MakeWindowKey(1, 99);
    const PreviewConfigKey region = MakeRegionKey(0, 42, 100, 200, 800, 600);
    EXPECT_TRUE(NeedsPreviewRestart(region, window));
}

// Transition 6: Region A → Region B with different origin replaces coordinates.
TEST(NeedsPreviewRestart, RegionAToRegionBDifferentOriginRequiresRestart) {
    const PreviewConfigKey region_a = MakeRegionKey(0, 42, 100, 200, 800, 600);
    const PreviewConfigKey region_b = MakeRegionKey(0, 42, 300, 400, 800, 600); // same size, different offset
    EXPECT_TRUE(NeedsPreviewRestart(region_b, region_a));
}

// Transition 7: Region A → differently sized Region B updates source dimensions.
TEST(NeedsPreviewRestart, RegionAToRegionBDifferentSizeRequiresRestart) {
    const PreviewConfigKey region_a = MakeRegionKey(0, 42, 100, 200, 800, 600);
    const PreviewConfigKey region_b = MakeRegionKey(0, 42, 100, 200, 1280, 720); // same offset, different size
    EXPECT_TRUE(NeedsPreviewRestart(region_b, region_a));
}

// Transition 8: Region → Display restores full source dimensions.
// Represented by the same key check as Transition 2 — the restart clears crop.
TEST(NeedsPreviewRestart, RegionToDisplayRestoresFullDimensions) {
    const PreviewConfigKey region = MakeRegionKey(0, 42, 0, 0, 1280, 720);
    const PreviewConfigKey display = MakeDisplayKey(0, 42); // full display, no crop
    // has_crop changed → restart needed → renderer recreated with nullopt crop_box.
    EXPECT_TRUE(NeedsPreviewRestart(display, region));
}

// Transition 9: Same Region reapplied — idempotent, no restart needed.
TEST(NeedsPreviewRestart, SameRegionReappliedIsIdempotent) {
    const PreviewConfigKey region_a = MakeRegionKey(0, 42, 100, 200, 800, 600);
    const PreviewConfigKey region_b = MakeRegionKey(0, 42, 100, 200, 800, 600); // identical
    EXPECT_FALSE(NeedsPreviewRestart(region_b, region_a));
}

// Transition 9b: Same Display reapplied — no restart needed.
TEST(NeedsPreviewRestart, SameDisplayReappliedIsIdempotent) {
    const PreviewConfigKey display = MakeDisplayKey(0, 42);
    EXPECT_FALSE(NeedsPreviewRestart(display, display));
}

// Transition 10: Invalid region (no valid crop box) produces has_crop=false,
// which differs from an active Region key → restart required to clear crop.
TEST(NeedsPreviewRestart, InvalidRegionKeyDiffersFromActiveRegion) {
    // A key representing "Region mode but no valid crop yet" has has_crop=false.
    PreviewConfigKey invalid_region;
    invalid_region.target_index = 0;
    invalid_region.native_id = 42;
    invalid_region.kind = 0;
    invalid_region.has_crop = false; // crop couldn't be computed (invalid region)

    const PreviewConfigKey active_region = MakeRegionKey(0, 42, 100, 200, 800, 600);

    // Switching from active Region to invalid-region state → restart needed to clear preview.
    EXPECT_TRUE(NeedsPreviewRestart(invalid_region, active_region));
}

// Transition: switching to different native_id on same index requires restart.
TEST(NeedsPreviewRestart, DifferentNativeIdRequiresRestart) {
    const PreviewConfigKey mon_a = MakeDisplayKey(0, 111);
    const PreviewConfigKey mon_b = MakeDisplayKey(0, 222); // same index, different monitor handle
    EXPECT_TRUE(NeedsPreviewRestart(mon_b, mon_a));
}

// ---------------------------------------------------------------------------
// Display 2 region crop — regression for black preview on secondary monitor
//
// Root cause: if a region on Display 2 (origin 2560,0) is rendered with the
// crop box computed relative to Display 1 (origin 0,0), the x offset equals
// the full virtual-screen x (e.g. 2700), which lies outside Display 1's
// 2560-pixel-wide frame. The D3D11 CopySubresourceRegion clamp produces
// zero-width/height copies → black pixels.
//
// Correct behaviour: the crop box must be computed using the origin of the
// monitor that the region's top-left corner belongs to (Display 2 in this
// case), not the currently-selected monitor (which may still be Display 1).
// ---------------------------------------------------------------------------

// A region on Display 2 (top-left at virtual-screen x=2700) is out-of-range
// when mistakenly computed relative to Display 1 (origin 0,0, width 2560).
TEST(RegionToCropBox, Display2RegionRelativeToDisplay1OriginIsOutOfRange) {
    // Display 2 sits at virtual-screen x=2560.  Region top-left is at (2700, 100).
    // Incorrectly using Display 1 origin (0, 0):
    const PreviewCropBox wrong_box = RegionToCropBox(2700, 100, 800, 600, /*monitor_origin*/ 0, 0);
    // crop x = 2700 - 0 = 2700, which exceeds a typical 2560-pixel Display 1 width.
    EXPECT_GT(wrong_box.x, 2560) << "Box x must exceed Display 1 width when wrong origin is used";
    // IsValid only checks non-negative coords and positive dims; it is still
    // technically valid here, but the D3D clamp would produce an empty copy.
    // The caller (startPreviewIfIdle) must select the correct monitor first.
}

// Same region, now correctly computed relative to Display 2 origin (2560, 0).
TEST(RegionToCropBox, Display2RegionRelativeToDisplay2OriginIsInRange) {
    // Region at virtual-screen (2700, 100, 800, 600); Display 2 origin = (2560, 0).
    const PreviewCropBox correct_box = RegionToCropBox(2700, 100, 800, 600, /*monitor_origin*/ 2560, 0);
    EXPECT_EQ(correct_box.x, 140);
    EXPECT_EQ(correct_box.y, 100);
    EXPECT_EQ(correct_box.width, 800);
    EXPECT_EQ(correct_box.height, 600);
    EXPECT_TRUE(correct_box.IsValid());
}

// The correct preview config key for a Display 2 region must carry the
// Display 2 monitor's target_index and native_id, not Display 1's.
// This ensures NeedsPreviewRestart detects the monitor change correctly when
// the user switches from a Display 1 region to a Display 2 region.
TEST(NeedsPreviewRestart, Display1RegionToDisplay2RegionRequiresRestartAndDifferentTarget) {
    // Display 1 region (index=0, native_id=111)
    const PreviewConfigKey display1_region = MakeRegionKey(0, 111, 100, 100, 800, 600);
    // Display 2 region (index=1, native_id=222) — different target AND different crop
    const PreviewConfigKey display2_region = MakeRegionKey(1, 222, 2700, 100, 800, 600);
    EXPECT_TRUE(NeedsPreviewRestart(display2_region, display1_region));
}

} // namespace
} // namespace exosnap
