// Pins the cursor-sprite clip/scale arithmetic (cursor_sprite.h) shared by the
// recording compositor's two cursor paths and the DXGI preview's sprite. The
// clip semantics were extracted verbatim from the compositor: a negative
// origin crops into the bitmap, the target edge crops the extent, and a sprite
// wider than 256 px per axis is rejected as malformed.

#include <exosnap/engine/cursor_sprite.h>

#include <gtest/gtest.h>

using namespace exosnap::engine;

TEST(CursorSpriteClip, FullyInsidePassesThrough) {
    const CursorSpriteClip c = ClipCursorSprite(10, 20, 32, 32, 1920, 1080);
    EXPECT_TRUE(c.visible);
    EXPECT_EQ(c.x, 10);
    EXPECT_EQ(c.y, 20);
    EXPECT_EQ(c.w, 32);
    EXPECT_EQ(c.h, 32);
    EXPECT_EQ(c.bitmap_off_x, 0);
    EXPECT_EQ(c.bitmap_off_y, 0);
}

TEST(CursorSpriteClip, NegativeOriginCropsIntoBitmap) {
    const CursorSpriteClip c = ClipCursorSprite(-8, -4, 32, 32, 1920, 1080);
    EXPECT_TRUE(c.visible);
    EXPECT_EQ(c.x, 0);
    EXPECT_EQ(c.y, 0);
    EXPECT_EQ(c.w, 24);
    EXPECT_EQ(c.h, 28);
    EXPECT_EQ(c.bitmap_off_x, 8);
    EXPECT_EQ(c.bitmap_off_y, 4);
}

TEST(CursorSpriteClip, TargetEdgeCropsExtent) {
    const CursorSpriteClip c = ClipCursorSprite(1900, 1070, 32, 32, 1920, 1080);
    EXPECT_TRUE(c.visible);
    EXPECT_EQ(c.w, 20);
    EXPECT_EQ(c.h, 10);
    EXPECT_EQ(c.bitmap_off_x, 0);
    EXPECT_EQ(c.bitmap_off_y, 0);
}

TEST(CursorSpriteClip, FullyOutsideIsInvisible) {
    EXPECT_FALSE(ClipCursorSprite(1920, 0, 32, 32, 1920, 1080).visible); // off right
    EXPECT_FALSE(ClipCursorSprite(-32, 0, 32, 32, 1920, 1080).visible);  // off left
    EXPECT_FALSE(ClipCursorSprite(0, -32, 32, 32, 1920, 1080).visible);  // off top
    EXPECT_FALSE(ClipCursorSprite(0, 1080, 32, 32, 1920, 1080).visible); // off bottom
}

TEST(CursorSpriteClip, MalformedSpriteRejected) {
    EXPECT_FALSE(ClipCursorSprite(0, 0, 0, 32, 1920, 1080).visible);
    EXPECT_FALSE(ClipCursorSprite(0, 0, 257, 32, 1920, 1080).visible);
    EXPECT_FALSE(ClipCursorSprite(0, 0, 32, 257, 1920, 1080).visible);
    EXPECT_FALSE(ClipCursorSprite(0, 0, 32, 32, 0, 1080).visible);
}

TEST(CursorSpritePlace, IdentityScaleKeepsPixels) {
    const CursorSpriteDraw d = PlaceCursorSprite(100, 50, 32, 32, 1920, 1080, 0.0f, 0.0f, 1920.0f, 1080.0f);
    EXPECT_TRUE(d.visible);
    EXPECT_FLOAT_EQ(d.dst_x, 100.0f);
    EXPECT_FLOAT_EQ(d.dst_y, 50.0f);
    EXPECT_FLOAT_EQ(d.dst_w, 32.0f);
    EXPECT_FLOAT_EQ(d.dst_h, 32.0f);
}

TEST(CursorSpritePlace, ContainFitScalesAndOffsets) {
    // A 3840x2160 frame drawn into a 960x540 content rect at (10, 20): quarter
    // scale, content offset added after scaling.
    const CursorSpriteDraw d = PlaceCursorSprite(400, 800, 64, 64, 3840, 2160, 10.0f, 20.0f, 960.0f, 540.0f);
    EXPECT_TRUE(d.visible);
    EXPECT_FLOAT_EQ(d.dst_x, 10.0f + 100.0f);
    EXPECT_FLOAT_EQ(d.dst_y, 20.0f + 200.0f);
    EXPECT_FLOAT_EQ(d.dst_w, 16.0f);
    EXPECT_FLOAT_EQ(d.dst_h, 16.0f);
}

TEST(CursorSpritePlace, ClipHappensInSourceSpace) {
    // Sprite hangs off the source's right edge: the crop is integer in source
    // pixels, the scaled destination shrinks with it.
    const CursorSpriteDraw d = PlaceCursorSprite(1900, 0, 32, 32, 1920, 1080, 0.0f, 0.0f, 960.0f, 540.0f);
    EXPECT_TRUE(d.visible);
    EXPECT_EQ(d.clip.w, 20);
    EXPECT_FLOAT_EQ(d.dst_w, 10.0f);
    EXPECT_FLOAT_EQ(d.dst_x, 950.0f);
}

TEST(CursorSpritePlace, DegenerateContentRectIsInvisible) {
    EXPECT_FALSE(PlaceCursorSprite(0, 0, 32, 32, 1920, 1080, 0.0f, 0.0f, 0.0f, 540.0f).visible);
}

TEST(ScaleCoordinate, RoundsToNearestAndPassesThroughUnknownBounds) {
    // 150 in a 300-wide bounds onto a 100-wide source -> 50.
    EXPECT_EQ(ScaleCoordinateToSource(150, 100, 300), 50);
    // Rounds to nearest: 5/3 -> 2.
    EXPECT_EQ(ScaleCoordinateToSource(5, 100, 300), 2);
    // Negative deltas round symmetrically.
    EXPECT_EQ(ScaleCoordinateToSource(-5, 100, 300), -2);
    // Unknown bounds pass the delta through.
    EXPECT_EQ(ScaleCoordinateToSource(42, 0, 300), 42);
    EXPECT_EQ(ScaleCoordinateToSource(42, 100, 0), 42);
}
