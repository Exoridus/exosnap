#include <gtest/gtest.h>

#include <recorder_core/webcam_placement.h>

#include <cmath>

namespace recorder_core {
namespace {

// Helper: assert a mapped rect is entirely inside the content rectangle.
void ExpectInsideContent(const WebcamPixelRect& r, int cx, int cy, int cw, int ch) {
    EXPECT_GE(r.x, cx);
    EXPECT_GE(r.y, cy);
    EXPECT_GT(r.w, 0);
    EXPECT_GT(r.h, 0);
    EXPECT_LE(r.x + r.w, cx + cw);
    EXPECT_LE(r.y + r.h, cy + ch);
}

// 1. Default placement is valid and bounds-safe.
TEST(WebcamPlacementTest, DefaultPlacementIsValid) {
    const WebcamPlacement p = SanitizeWebcamPlacement(WebcamPlacement{});
    EXPECT_GE(p.x, 0.0f);
    EXPECT_GE(p.y, 0.0f);
    EXPECT_GE(p.w, WebcamPlacement::kMinSize);
    EXPECT_GE(p.h, WebcamPlacement::kMinSize);
    EXPECT_LE(p.x + p.w, 1.0f);
    EXPECT_LE(p.y + p.h, 1.0f);
}

// 2. Placement maps correctly onto a 16:9 output.
TEST(WebcamPlacementTest, MapsTo16x9Output) {
    const WebcamPlacement p{0.75f, 0.75f, 0.25f, 0.25f, false};
    const WebcamPixelRect r = MapWebcamPlacementToContent(p, 0, 0, 1920, 1080);
    EXPECT_EQ(r.x, 1440);
    EXPECT_EQ(r.y, 810);
    EXPECT_EQ(r.w, 480);
    EXPECT_EQ(r.h, 270);
    ExpectInsideContent(r, 0, 0, 1920, 1080);
}

// 3. Placement maps correctly onto a portrait output.
TEST(WebcamPlacementTest, MapsToPortraitOutput) {
    const WebcamPlacement p{0.10f, 0.10f, 0.30f, 0.20f, false};
    const WebcamPixelRect r = MapWebcamPlacementToContent(p, 0, 0, 1080, 1920);
    EXPECT_EQ(r.x, 108);
    EXPECT_EQ(r.y, 192);
    EXPECT_EQ(r.w, 324);
    EXPECT_EQ(r.h, 384);
    ExpectInsideContent(r, 0, 0, 1080, 1920);
}

// 4. Placement maps correctly onto an arbitrary content rect (non-zero origin).
TEST(WebcamPlacementTest, MapsToArbitraryContentRect) {
    const WebcamPlacement p{0.50f, 0.50f, 0.25f, 0.25f, false};
    const WebcamPixelRect r = MapWebcamPlacementToContent(p, 100, 50, 800, 600);
    EXPECT_EQ(r.x, 100 + 400);
    EXPECT_EQ(r.y, 50 + 300);
    EXPECT_EQ(r.w, 200);
    EXPECT_EQ(r.h, 150);
    ExpectInsideContent(r, 100, 50, 800, 600);
}

// 5. Letterbox margins do not change placement: position is relative to the content
//    rect, not the surrounding widget. Two content rects of equal size but different
//    origin yield the same in-content offset ratio.
TEST(WebcamPlacementTest, LetterboxMarginsDoNotAffectOutputPlacement) {
    const WebcamPlacement p{0.40f, 0.30f, 0.20f, 0.20f, false};
    const WebcamPixelRect full = MapWebcamPlacementToContent(p, 0, 0, 1200, 900);
    const WebcamPixelRect boxed = MapWebcamPlacementToContent(p, 200, 0, 1200, 900); // pillarboxed
    EXPECT_EQ(full.x - 0, boxed.x - 200);
    EXPECT_EQ(full.y, boxed.y);
    EXPECT_EQ(full.w, boxed.w);
    EXPECT_EQ(full.h, boxed.h);
}

// 6. Move preserves size: same w/h with different x/y yields identical pixel size.
TEST(WebcamPlacementTest, MovePreservesSize) {
    const WebcamPlacement a{0.10f, 0.10f, 0.25f, 0.25f, false};
    const WebcamPlacement b{0.60f, 0.55f, 0.25f, 0.25f, false};
    const WebcamPixelRect ra = MapWebcamPlacementToContent(a, 0, 0, 1920, 1080);
    const WebcamPixelRect rb = MapWebcamPlacementToContent(b, 0, 0, 1920, 1080);
    EXPECT_EQ(ra.w, rb.w);
    EXPECT_EQ(ra.h, rb.h);
}

// 15. Resolution / DPI independence: the same normalized placement scales
//     proportionally with the content size.
TEST(WebcamPlacementTest, ResolutionIndependentScaling) {
    const WebcamPlacement p{0.25f, 0.25f, 0.50f, 0.50f, false};
    const WebcamPixelRect small = MapWebcamPlacementToContent(p, 0, 0, 1280, 720);
    const WebcamPixelRect large = MapWebcamPlacementToContent(p, 0, 0, 2560, 1440);
    EXPECT_EQ(large.x, small.x * 2);
    EXPECT_EQ(large.y, small.y * 2);
    EXPECT_EQ(large.w, small.w * 2);
    EXPECT_EQ(large.h, small.h * 2);
}

// 10. Minimum size enforced.
TEST(WebcamPlacementTest, MinimumSizeEnforced) {
    const WebcamPlacement p = SanitizeWebcamPlacement(WebcamPlacement{0.5f, 0.5f, 0.001f, 0.001f, false});
    EXPECT_GE(p.w, WebcamPlacement::kMinSize);
    EXPECT_GE(p.h, WebcamPlacement::kMinSize);
    const WebcamPixelRect r = MapWebcamPlacementToContent(p, 0, 0, 1920, 1080);
    EXPECT_GE(r.w, 1);
    EXPECT_GE(r.h, 1);
}

// 11. Maximum size enforced (clamped to the content frame).
TEST(WebcamPlacementTest, MaximumSizeClampedToFrame) {
    const WebcamPlacement p = SanitizeWebcamPlacement(WebcamPlacement{0.0f, 0.0f, 5.0f, 5.0f, false});
    EXPECT_LE(p.w, WebcamPlacement::kMaxSize);
    EXPECT_LE(p.h, WebcamPlacement::kMaxSize);
    const WebcamPixelRect r = MapWebcamPlacementToContent(p, 0, 0, 1920, 1080);
    ExpectInsideContent(r, 0, 0, 1920, 1080);
}

// 7 + bounds: negative coordinates are clamped; the rect never escapes content.
TEST(WebcamPlacementTest, NegativeCoordinatesClamped) {
    const WebcamPlacement p = SanitizeWebcamPlacement(WebcamPlacement{-0.5f, -0.5f, 0.25f, 0.25f, false});
    EXPECT_GE(p.x, 0.0f);
    EXPECT_GE(p.y, 0.0f);
    const WebcamPixelRect r = MapWebcamPlacementToContent(p, 0, 0, 1920, 1080);
    ExpectInsideContent(r, 0, 0, 1920, 1080);
}

// 12. Overflow does not invert or escape: x+w beyond 1 is clamped inside.
TEST(WebcamPlacementTest, OverflowClampedNoInversion) {
    const WebcamPlacement p = SanitizeWebcamPlacement(WebcamPlacement{0.90f, 0.90f, 0.50f, 0.50f, false});
    EXPECT_LE(p.x + p.w, 1.0f + 1e-5f);
    EXPECT_LE(p.y + p.h, 1.0f + 1e-5f);
    const WebcamPixelRect r = MapWebcamPlacementToContent(p, 0, 0, 1920, 1080);
    EXPECT_GT(r.w, 0);
    EXPECT_GT(r.h, 0);
    ExpectInsideContent(r, 0, 0, 1920, 1080);
}

// Non-finite inputs are repaired to safe defaults.
TEST(WebcamPlacementTest, NonFiniteRepaired) {
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const WebcamPlacement p = SanitizeWebcamPlacement(WebcamPlacement{inf, -inf, nan, nan, false});
    EXPECT_TRUE(std::isfinite(p.x));
    EXPECT_TRUE(std::isfinite(p.y));
    EXPECT_FLOAT_EQ(p.w, 0.25f);
    EXPECT_FLOAT_EQ(p.h, 0.25f);
}

// 32. Mirror flag does not alter placement dimensions.
TEST(WebcamPlacementTest, MirrorDoesNotChangeDimensions) {
    const WebcamPlacement plain{0.4f, 0.4f, 0.3f, 0.3f, false};
    WebcamPlacement mirrored = plain;
    mirrored.mirror = true;
    const WebcamPixelRect a = MapWebcamPlacementToContent(plain, 0, 0, 1920, 1080);
    const WebcamPixelRect b = MapWebcamPlacementToContent(mirrored, 0, 0, 1920, 1080);
    EXPECT_EQ(a.x, b.x);
    EXPECT_EQ(a.y, b.y);
    EXPECT_EQ(a.w, b.w);
    EXPECT_EQ(a.h, b.h);
}

// Degenerate content yields an invalid rect (callers skip drawing).
TEST(WebcamPlacementTest, ZeroContentYieldsInvalidRect) {
    const WebcamPixelRect r = MapWebcamPlacementToContent(WebcamPlacement{}, 0, 0, 0, 0);
    EXPECT_FALSE(r.IsValid());
}

// 34 + 35. Preview and output use the same normalized placement; their pixel rects
// differ only by deterministic scaling between the preview content rect and the
// encode frame.
TEST(WebcamPlacementTest, PreviewAndOutputDifferOnlyByScaling) {
    const WebcamPlacement p{0.60f, 0.65f, 0.25f, 0.20f, true};
    // Preview content rect (e.g. letterboxed 16:9 inside a widget) and the encode frame
    // share the same aspect ratio, so the normalized placement lands at the same
    // relative position in both.
    const WebcamPixelRect preview = MapWebcamPlacementToContent(p, 32, 18, 640, 360);
    const WebcamPixelRect output = MapWebcamPlacementToContent(p, 0, 0, 1920, 1080);
    const double scale = 1920.0 / 640.0;
    EXPECT_NEAR((preview.x - 32) * scale, output.x, 2.0);
    EXPECT_NEAR((preview.y - 18) * scale, output.y, 2.0);
    EXPECT_NEAR(preview.w * scale, output.w, 2.0);
    EXPECT_NEAR(preview.h * scale, output.h, 2.0);
}

} // namespace
} // namespace recorder_core
