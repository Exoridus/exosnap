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

// The four states a mask-only Win32 cursor's pixel can be in. The AND mask
// selects whether the destination survives, and the XOR mask is applied after
// it, which is why two of the four are not colours at all.
//
//   AND XOR  result
//    0   0   opaque black
//    0   1   opaque white
//    1   0   destination unchanged
//    1   1   destination inverted
//
// Only the first three fit a colour with an alpha. The fourth is what keeps the
// classic I-beam legible on a light and a dark background alike, and a sprite
// that drops it is invisible on every cursor built from it alone.
enum class Win32CursorMaskState {
    OpaqueBlack,
    OpaqueWhite,
    Transparent,
    Invert,
};

[[nodiscard]] constexpr Win32CursorMaskState Win32CursorMaskStateOf(bool and_bit, bool xor_bit) noexcept {
    if (!and_bit) {
        return xor_bit ? Win32CursorMaskState::OpaqueWhite : Win32CursorMaskState::OpaqueBlack;
    }
    return xor_bit ? Win32CursorMaskState::Invert : Win32CursorMaskState::Transparent;
}

// A cursor image captured from an HCURSOR: tightly packed BGRA with the
// hotspot the position points at.
//
// `invert` is the second plane a mask cursor needs, tightly packed BGRA of the
// same extent: opaque white where the pixel inverts its destination, zero
// everywhere else. It is empty for a cursor that inverts nothing, which is every
// colour cursor and most mask ones. Its shape suits the blend that draws it --
// SrcBlend INV_DEST_COLOR against DestBlend INV_SRC_ALPHA leaves a zero pixel's
// destination exactly as it was, so no pixel has to be clipped out.
struct Win32CursorBitmap {
    std::vector<uint8_t> bgra;
    std::vector<uint8_t> invert;
    int width = 0;
    int height = 0;
    int hotspot_x = 0;
    int hotspot_y = 0;
};

// Render the cursor into a BGRA bitmap via GetIconInfo + DrawIconEx. Returns
// false for a null cursor, a degenerate size (0 or > 256 px), or a GDI
// failure; `out` is only written on success.
//
// For a mask-only cursor the alpha channel is rebuilt from the mask, because
// DrawIconEx writes none: the three colour states become opaque black, opaque
// white and transparent, and the inverting state is reported separately in
// `out.invert` rather than forced into one of them.
bool CaptureWin32CursorBitmap(HCURSOR cursor, Win32CursorBitmap& out);

// Why a cursor sample produced no sprite. An absent pointer is a normal state
// and every one of these paths is silent in the capture loop, which leaves a
// desktop with nothing to draw indistinguishable from a sprite that would not
// rasterize. Only NotShowing and NullHandle are the operating system declining
// to offer a cursor; the rest are the recorder failing to use one it was given.
enum class WgcCursorSampleOutcome {
    Sampled,
    CursorInfoFailed,
    NotShowing,
    NullHandle,
    SpriteCaptureFailed,
    BoundsEmpty,
    // The per-monitor-aware scope the position and the bounds have to share could
    // not be entered, so the two would have been read in different spaces. The
    // sample is skipped rather than placed somewhere the pointer never was.
    DpiScopeUnavailable,
};

// Classify the CURSORINFO half of a sample. Split out from the capture loop so
// the precedence is pinned: a failed query reports neither visibility nor a
// handle, so it must not be reported as a hidden cursor.
[[nodiscard]] constexpr WgcCursorSampleOutcome ClassifyWgcCursorInfo(bool info_ok, bool showing,
                                                                     bool has_handle) noexcept {
    if (!info_ok) {
        return WgcCursorSampleOutcome::CursorInfoFailed;
    }
    if (!showing) {
        return WgcCursorSampleOutcome::NotShowing;
    }
    if (!has_handle) {
        return WgcCursorSampleOutcome::NullHandle;
    }
    return WgcCursorSampleOutcome::Sampled;
}

[[nodiscard]] constexpr const char* WgcCursorSampleOutcomeName(WgcCursorSampleOutcome outcome) noexcept {
    switch (outcome) {
    case WgcCursorSampleOutcome::CursorInfoFailed:
        return "cursor_info_failed";
    case WgcCursorSampleOutcome::NotShowing:
        return "not_showing";
    case WgcCursorSampleOutcome::NullHandle:
        return "null_cursor_handle";
    case WgcCursorSampleOutcome::SpriteCaptureFailed:
        return "sprite_capture_failed";
    case WgcCursorSampleOutcome::BoundsEmpty:
        return "cursor_bounds_empty";
    case WgcCursorSampleOutcome::DpiScopeUnavailable:
        return "dpi_scope_unavailable";
    case WgcCursorSampleOutcome::Sampled:
        break;
    }
    return "sampled";
}

// Map a screen-space delta into source-texture pixels when the captured
// bounds and the source texture differ in size (DPI-scaled window capture).
// Rounds to nearest; passes the delta through when either extent is unknown.
int32_t ScaleCoordinateToSource(int32_t screen_delta, int32_t source_pixels, int32_t bounds_pixels) noexcept;

// The sprite's top-left in source pixels for a pointer at `screen` over a window
// occupying `bounds`, with the sprite's hotspot at the pointer.
//
// Both arguments must come from the same coordinate space. They do not by
// default: a process that is not per-monitor DPI aware is handed a pointer
// position virtualised for the monitor the pointer is over, while a window on an
// unscaled monitor reports unvirtualised bounds. Subtracting one from the other
// compresses whatever part of the distance lies on a scaled monitor and leaves the
// rest alone, so the sprite lands short by an amount that depends on where the
// pointer is. The caller reads both under one per-monitor-aware scope for that
// reason; this function cannot detect the mistake, only inherit it.

// The sprite's hotspot-adjusted top-left, in source pixels. Pure, and the single
// place the two halves of the mapping meet, so a test can pin what happens when
// they disagree about their coordinate space.
struct CursorSourcePoint {
    int32_t x = 0;
    int32_t y = 0;
};

[[nodiscard]] inline CursorSourcePoint MapCursorToSource(int32_t screen_x, int32_t screen_y, const RECT& bounds,
                                                         int32_t source_width, int32_t source_height, int32_t hotspot_x,
                                                         int32_t hotspot_y) noexcept {
    const int32_t bounds_w = bounds.right - bounds.left;
    const int32_t bounds_h = bounds.bottom - bounds.top;
    CursorSourcePoint point;
    point.x = ScaleCoordinateToSource(screen_x - bounds.left, source_width, bounds_w) - hotspot_x;
    point.y = ScaleCoordinateToSource(screen_y - bounds.top, source_height, bounds_h) - hotspot_y;
    return point;
}

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
