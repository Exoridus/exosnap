#pragma once

#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Canonical webcam PiP placement model + the single shared coordinate-mapping
// helper used by every consumer:
//   * the UI Record preview (Qt overlay / DXGI overlay) — content rect = the
//     contain-fit source rectangle inside the preview widget (no letterbox),
//   * the recording compositor (video_thread) — content rect = the full encode
//     frame at origin (0,0).
//
// Keeping the math here (Qt-free, header-only) guarantees Preview and recorded
// output agree on placement, size and mirror.  Do NOT re-derive this mapping in
// UI or compositor code — call MapWebcamPlacementToContent instead.
// ---------------------------------------------------------------------------

namespace recorder_core {

// Normalized placement as fractions [0,1] of the *content* rectangle.  This is
// deliberately independent of widget pixels, DPI and output resolution: the same
// WebcamPlacement maps deterministically onto any content rectangle.
struct WebcamPlacement {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.25f;
    float h = 0.25f;
    bool mirror = false;

    // Minimum / maximum normalized side length.  The minimum keeps the PiP
    // grabbable; the maximum prevents it from exceeding the content frame.
    static constexpr float kMinSize = 0.05f;
    static constexpr float kMaxSize = 1.0f;
};

// Integer pixel rectangle inside a content rectangle.
struct WebcamPixelRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return w > 0 && h > 0;
    }
};

namespace detail {

[[nodiscard]] inline float FiniteOr(float value, float fallback) noexcept {
    return std::isfinite(static_cast<double>(value)) ? value : fallback;
}

} // namespace detail

// Clamp a placement so it is entirely inside [0,1] with the min/max side length
// enforced and no negative or inverted geometry.  Non-finite inputs fall back to
// safe defaults.  Deterministic and resolution-independent.
[[nodiscard]] inline WebcamPlacement SanitizeWebcamPlacement(WebcamPlacement p) noexcept {
    float x = detail::FiniteOr(p.x, 0.0f);
    float y = detail::FiniteOr(p.y, 0.0f);
    float w = detail::FiniteOr(p.w, 0.25f);
    float h = detail::FiniteOr(p.h, 0.25f);

    w = std::clamp(w, WebcamPlacement::kMinSize, WebcamPlacement::kMaxSize);
    h = std::clamp(h, WebcamPlacement::kMinSize, WebcamPlacement::kMaxSize);
    x = std::clamp(x, 0.0f, 1.0f - WebcamPlacement::kMinSize);
    y = std::clamp(y, 0.0f, 1.0f - WebcamPlacement::kMinSize);

    if (x + w > 1.0f) {
        w = (std::max)(WebcamPlacement::kMinSize, 1.0f - x);
    }
    if (y + h > 1.0f) {
        h = (std::max)(WebcamPlacement::kMinSize, 1.0f - y);
    }
    if (x + w > 1.0f) {
        x = (std::max)(0.0f, 1.0f - w);
    }
    if (y + h > 1.0f) {
        y = (std::max)(0.0f, 1.0f - h);
    }

    p.x = x;
    p.y = y;
    p.w = w;
    p.h = h;
    return p;
}

// Map a (sanitized) normalized placement onto the given content rectangle,
// returning a bounds-clamped integer pixel rect that never escapes the content
// frame.  `content_x/content_y` is the top-left of the content area inside the
// target surface (for a letterboxed preview this is the contain-fit offset; for
// the recording encode frame it is 0,0).
//
// This is the canonical preview<->output conversion: feeding the same placement
// with the preview content rect and with the encode frame guarantees identical
// relative geometry.
[[nodiscard]] inline WebcamPixelRect MapWebcamPlacementToContent(const WebcamPlacement& placement, int content_x,
                                                                 int content_y, int content_w, int content_h) noexcept {
    WebcamPixelRect rect;
    if (content_w <= 0 || content_h <= 0) {
        return rect;
    }

    const WebcamPlacement p = SanitizeWebcamPlacement(placement);
    const double cw = static_cast<double>(content_w);
    const double ch = static_cast<double>(content_h);

    int w = static_cast<int>(std::lround(p.w * cw));
    int h = static_cast<int>(std::lround(p.h * ch));
    w = std::clamp(w, 1, content_w);
    h = std::clamp(h, 1, content_h);

    int x = content_x + static_cast<int>(std::lround(p.x * cw));
    int y = content_y + static_cast<int>(std::lround(p.y * ch));
    // Keep the whole rect inside [content_x, content_x + content_w).
    x = std::clamp(x, content_x, content_x + content_w - w);
    y = std::clamp(y, content_y, content_y + content_h - h);

    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    return rect;
}

} // namespace recorder_core
