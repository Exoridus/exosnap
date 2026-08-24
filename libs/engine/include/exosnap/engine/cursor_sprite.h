#pragma once

// ---------------------------------------------------------------------------
// Cursor sprite capture and placement, shared by every consumer that draws a
// mouse cursor over a captured frame:
//   * the recording compositor's OD pointer-shape path (video_thread.cpp),
//   * the recording compositor's Win32 fallback for WGC window capture,
//   * the DXGI live preview, whose idle DXGI-hub frames carry no cursor
//     (Output Duplication composites none; the engine adds it only while
//     recording).
//
// The placement math is pure and unit-pinned; the bitmap capture wraps
// GetIconInfo/DrawIconEx. Do NOT re-derive the clip arithmetic at a call
// site — the sprite the preview draws must land on the same pixels the
// encoder writes.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <vector>

#include <windows.h>

namespace exosnap::engine {

// A cursor image captured from an HCURSOR: tightly packed BGRA with the
// hotspot the position points at.
struct Win32CursorBitmap {
    std::vector<uint8_t> bgra;
    int width = 0;
    int height = 0;
    int hotspot_x = 0;
    int hotspot_y = 0;
};

// Render the cursor into a BGRA bitmap via GetIconInfo + DrawIconEx. Returns
// false for a null cursor, a degenerate size (0 or > 256 px), or a GDI
// failure; `out` is only written on success.
bool CaptureWin32CursorBitmap(HCURSOR cursor, Win32CursorBitmap& out);

// Map a screen-space delta into source-texture pixels when the captured
// bounds and the source texture differ in size (DPI-scaled window capture).
// Rounds to nearest; passes the delta through when either extent is unknown.
int32_t ScaleCoordinateToSource(int32_t screen_delta, int32_t source_pixels, int32_t bounds_pixels) noexcept;

// A cursor sprite clipped against the target it is drawn into. Pure.
struct CursorSpriteClip {
    bool visible = false; // false: fully outside the target, or degenerate
    int32_t x = 0;        // clipped top-left in target pixels
    int32_t y = 0;
    int32_t w = 0; // clipped extent (<= bitmap extent)
    int32_t h = 0;
    int32_t bitmap_off_x = 0; // top-left crop into the cursor bitmap
    int32_t bitmap_off_y = 0;
};

// Clip a cursor bitmap of bmp_w x bmp_h placed with its (hotspot-adjusted)
// top-left at (x, y) against a target_w x target_h surface. A sprite larger
// than 256 px per axis is rejected as malformed, matching the capture side.
[[nodiscard]] CursorSpriteClip ClipCursorSprite(int32_t x, int32_t y, int32_t bmp_w, int32_t bmp_h, int32_t target_w,
                                                int32_t target_h) noexcept;

// The clipped sprite scaled from source-frame space into a destination
// content rectangle (the preview's contain-fit rect). Pure.
struct CursorSpriteDraw {
    bool visible = false;
    CursorSpriteClip clip; // crop into the bitmap, in source pixels
    float dst_x = 0.0f;    // destination rect in target (content) coordinates
    float dst_y = 0.0f;
    float dst_w = 0.0f;
    float dst_h = 0.0f;
};

// Place a cursor bitmap whose (hotspot-adjusted) top-left sits at
// (src_x, src_y) in a src_w x src_h frame onto the content rectangle that
// frame is drawn into. Clips in source space first, then scales, so the crop
// into the bitmap stays integer while the destination may be fractional.
[[nodiscard]] CursorSpriteDraw PlaceCursorSprite(int32_t src_x, int32_t src_y, int32_t bmp_w, int32_t bmp_h,
                                                 int32_t src_w, int32_t src_h, float content_x, float content_y,
                                                 float content_w, float content_h) noexcept;

} // namespace exosnap::engine
