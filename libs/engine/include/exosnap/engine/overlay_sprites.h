#pragma once

#include <exosnap/engine/webcam_placement.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// ---------------------------------------------------------------------------
// Overlay sprites: any recorded overlay that is "an atlas plus quads".
//
// A sprite source owns one atlas image and decides, per composed frame, which
// rectangles of it appear where. The engine composites every sprite layer
// above the webcam, in the order the sources are listed in RecorderConfig, with
// the same overlay shader the cursor uses: sRGB-encoded colour, the sprite's
// own alpha, and on the native HDR10 path the overlay reference white.
//
// Qt-free and D3D-free, so a source can be implemented and tested anywhere.
// ---------------------------------------------------------------------------

namespace exosnap::engine {

// Immutable atlas image. Pixels are BGRA8 (B8G8R8A8 byte order), tightly
// packed (width * 4 bytes per row), sRGB-encoded and with STRAIGHT alpha: the
// overlay shader blends SRC_ALPHA / INV_SRC_ALPHA and decodes sRGB before the
// blend on the HDR path, so premultiplied input would darken every edge.
//
// Sampling is bilinear. A quad drawn at its atlas size samples texel centres
// exactly; a scaled one also reads the neighbouring texels, so separate atlas
// entries by at least one fully transparent pixel and fill transparent pixels
// with the colour of their opaque neighbours where a scaled edge must not fringe.
struct SpriteAtlas {
    int width = 0;
    int height = 0;
    // Identifies the pixels. A source must advance it whenever the pixels
    // change; the compositor uploads the atlas again only when it differs from
    // the last one drawn for the same layer.
    uint64_t generation = 0;
    std::vector<uint8_t> pixels;

    [[nodiscard]] bool HasCompletePayload() const noexcept {
        return width > 0 && height > 0 &&
               pixels.size() >= static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    }
};

// Source rectangle inside the atlas, in atlas pixels.
struct SpriteAtlasRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool operator==(const SpriteAtlasRect&) const = default;
};

// Destination rectangle as fractions of the content rectangle (the captured
// region before output scaling), the same space WebcamPlacement uses. It may
// extend past the content edges; the part outside is clipped, not squeezed in.
struct SpriteContentRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    bool operator==(const SpriteContentRect&) const = default;
};

struct SpriteQuad {
    SpriteAtlasRect source;
    SpriteContentRect destination;
    // Multiplied onto the sprite's alpha, clamped to [0,1]. A quad at 0 is skipped.
    float opacity = 1.0f;

    bool operator==(const SpriteQuad&) const = default;
};

// One layer as it should appear in one frame. Quads draw in list order, later
// quads on top.
struct SpriteLayerSnapshot {
    std::shared_ptr<const SpriteAtlas> atlas;
    std::vector<SpriteQuad> quads;
};

// Injected into RecorderConfig::sprite_sources. Not owned; it must outlive Record().
//
// Snapshot is called by the video thread once per composed frame, before the
// frame is compared against the previous one, with that frame's time on the
// QueryPerformanceCounter clock in 100 ns units (the CFR output slot, or the
// capture time of a VFR frame). Implementations must therefore:
//   * be thread-safe against their own producer threads, and never block on
//     them: take a short lock or an atomic pointer swap, nothing that waits on
//     I/O, Qt or a GPU;
//   * not touch Qt objects or D3D (no device is passed, by design);
//   * return quickly and allocate nothing in the steady state. Returning the
//     same shared snapshot again while nothing changed is the cheapest answer,
//     and lets the engine skip recomposition without comparing quads.
// A null result or an atlas without a complete payload draws nothing. The
// returned atlas and quads must not be modified after they are returned.
struct SpriteSourceProvider {
    virtual std::shared_ptr<const SpriteLayerSnapshot> Snapshot(uint64_t frame_time_qpc_100ns) = 0;
    virtual ~SpriteSourceProvider() = default;
};

// Whether `next` differs visually from `previous`. A held desktop frame is
// recomposited exactly when this is true, so it compares what is drawn: atlas
// identity (generation and size) and the quad list.
[[nodiscard]] inline bool SpriteLayerChanged(const SpriteLayerSnapshot* previous,
                                             const SpriteLayerSnapshot* next) noexcept {
    if (previous == next)
        return false;
    if (previous == nullptr || next == nullptr)
        return true;
    const SpriteAtlas* a = previous->atlas.get();
    const SpriteAtlas* b = next->atlas.get();
    if ((a == nullptr) != (b == nullptr))
        return true;
    if (a != nullptr && (a->generation != b->generation || a->width != b->width || a->height != b->height))
        return true;
    return previous->quads != next->quads;
}

// Where one quad lands on the composite and which part of the atlas fills it.
struct SpriteDraw {
    WebcamPixelRect destination;
    // Normalized atlas coordinates of the destination's left/top and
    // right/bottom edges.
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    float opacity = 1.0f;

    [[nodiscard]] bool IsValid() const noexcept {
        return destination.IsValid();
    }
};

// Maps a quad onto the content rectangle of the composite. The destination is
// rounded to whole pixels and clipped to the content rectangle; the atlas
// coordinates follow the clipped, rounded edges so the visible part keeps its
// scale. Returns an invalid draw for a quad that is malformed (non-finite,
// empty, source outside the atlas), fully transparent or entirely clipped.
[[nodiscard]] inline SpriteDraw MapSpriteQuad(const SpriteQuad& quad, int atlas_width, int atlas_height, int content_x,
                                              int content_y, int content_w, int content_h) noexcept {
    SpriteDraw draw;
    const SpriteAtlasRect& s = quad.source;
    const SpriteContentRect& d = quad.destination;
    if (atlas_width <= 0 || atlas_height <= 0 || content_w <= 0 || content_h <= 0)
        return draw;
    if (s.w <= 0 || s.h <= 0 || s.x < 0 || s.y < 0 || s.x > atlas_width - s.w || s.y > atlas_height - s.h)
        return draw;
    if (!std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.w) || !std::isfinite(d.h) || d.w <= 0.0f ||
        d.h <= 0.0f)
        return draw;
    const float opacity = std::isfinite(quad.opacity) ? std::clamp(quad.opacity, 0.0f, 1.0f) : 1.0f;
    if (opacity <= 0.0f)
        return draw;

    const double x0 = content_x + static_cast<double>(d.x) * content_w;
    const double y0 = content_y + static_cast<double>(d.y) * content_h;
    const double x1 = content_x + static_cast<double>(d.x + d.w) * content_w;
    const double y1 = content_y + static_cast<double>(d.y + d.h) * content_h;
    // Bounded before the integer conversion: a far off-screen quad must clip,
    // not overflow.
    const auto snap = [](double v, int lo, int hi) {
        return static_cast<int>(std::lround(std::clamp(v, static_cast<double>(lo), static_cast<double>(hi))));
    };
    const int px0 = snap(x0, content_x, content_x + content_w);
    const int py0 = snap(y0, content_y, content_y + content_h);
    const int px1 = snap(x1, content_x, content_x + content_w);
    const int py1 = snap(y1, content_y, content_y + content_h);
    if (px1 <= px0 || py1 <= py0)
        return draw;

    const auto atlas_u = [&](int px) { return static_cast<float>((s.x + (px - x0) / (x1 - x0) * s.w) / atlas_width); };
    const auto atlas_v = [&](int py) { return static_cast<float>((s.y + (py - y0) / (y1 - y0) * s.h) / atlas_height); };
    draw.destination = WebcamPixelRect{px0, py0, px1 - px0, py1 - py0};
    draw.u0 = atlas_u(px0);
    draw.v0 = atlas_v(py0);
    draw.u1 = atlas_u(px1);
    draw.v1 = atlas_v(py1);
    draw.opacity = opacity;
    return draw;
}

} // namespace exosnap::engine
