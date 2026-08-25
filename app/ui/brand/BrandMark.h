#pragma once

// The ExoSnap mark, as numbers.
//
// SOURCE OF TRUTH
// ---------------
// This header is the ONLY authoritative definition of the aperture geometry, of
// the optical corrections small shell sizes need, and of the transport glyph
// shapes. Three consumers read it and none of them keeps a second copy:
//
//   * ui/brand/ShellIconRenderer -- the runtime renderer behind the tray icon
//     and the tray menu glyphs.
//   * quick/ExoSnap/Quick/ExoBrandMark.qml -- the in-application mark, through
//     the geometry the renderer's provider publishes.
//   * scripts/generate-app-icons.py -- the build-time .ico and .svg generator,
//     which parses the constants below rather than restating them.
//
// The generator fails loudly if a name it needs is missing, so a rename here
// breaks the build instead of silently producing an icon that no longer matches
// the running application. `brand_geometry_tests` is the drift guard for the
// artefacts that are checked in.
//
// Anything added here must stay parseable: one `inline constexpr double kName =
// value;` per line, and designated initializers for the aggregates.

#include <QtGlobal>

namespace exosnap::ui::brand {

// ---------------------------------------------------------------------------
// The aperture
// ---------------------------------------------------------------------------
// A 32-unit square. Every radius, stroke and offset below is in those units, so
// a caller scales once and the weights stay in proportion.
inline constexpr double kGrid = 32.0;
inline constexpr double kCenter = 16.0;

inline constexpr double kOuterRadius = 14.5;
inline constexpr double kOuterStroke = 1.5;
inline constexpr double kOuterOpacity = 0.45;

inline constexpr double kInnerRadius = 6.2;
inline constexpr double kInnerStroke = 1.6;

inline constexpr double kDotRadius = 2.4;

// Inline in the UI the mark fills its box edge to edge. As a standalone icon it
// needs margin: Windows draws a tray icon hard against its neighbours and an
// application icon against the taskbar's own edges, and a full-bleed ring reads
// as a crop rather than as a circle.
inline constexpr double kStandaloneContentScale = 0.88;

// ---------------------------------------------------------------------------
// Optical sizing
// ---------------------------------------------------------------------------
// The same geometry, corrected for the size it is rasterized at. Not three
// logos: every profile draws the identical three circles, and only the stroke
// weights, the dot and the outer ring's alpha move.
//
// The correction exists because thin strokes do not survive downsampling. At
// 16 px the outer ring is 1.5/32 * 16 = 0.75 device pixels; antialiased down to
// three quarters of a pixel it turns into a grey suggestion of a circle, and the
// 2.4-unit dot lands at 1.2 px where it stops reading as a dot at all. The
// values below were chosen from rendered evidence at each size, not derived.
struct OpticalProfile {
    // Multipliers on the geometry above.
    double outer_stroke_scale;
    double inner_stroke_scale;
    // The inner ring moves outwards at small sizes. Everything else can be
    // corrected in place, but the aperture's inner void cannot: at 16 px the
    // gap between the dot and the inner ring is about one device pixel, and one
    // antialiased pixel of gap is not a gap. Pushing the ring out is the only
    // correction that restores the hole without shrinking the dot below the
    // point where it stops being a dot.
    double inner_radius_scale;
    double dot_radius_scale;
    double outer_opacity_scale;
    // On kStandaloneContentScale: a small icon can afford less margin, because
    // at 16 px the margin costs whole pixels the mark needs more.
    double content_scale;
};

// Where the profiles change over. Small covers the notification area and the
// Explorer list at 100 % DPI; Medium covers the ordinary shell sizes and the
// scaled small ones; Large is the brand geometry unmodified.
inline constexpr int kSmallProfileMaxPx = 20;
inline constexpr int kMediumProfileMaxPx = 48;

inline constexpr OpticalProfile kSmallProfile{
    .outer_stroke_scale = 1.55,
    .inner_stroke_scale = 1.15,
    .inner_radius_scale = 1.32,
    .dot_radius_scale = 1.00,
    .outer_opacity_scale = 1.60,
    .content_scale = 1.06,
};

inline constexpr OpticalProfile kMediumProfile{
    .outer_stroke_scale = 1.18,
    .inner_stroke_scale = 1.10,
    .inner_radius_scale = 1.06,
    .dot_radius_scale = 1.02,
    .outer_opacity_scale = 1.25,
    .content_scale = 1.02,
};

inline constexpr OpticalProfile kLargeProfile{
    .outer_stroke_scale = 1.0,
    .inner_stroke_scale = 1.0,
    .inner_radius_scale = 1.0,
    .dot_radius_scale = 1.0,
    .outer_opacity_scale = 1.0,
    .content_scale = 1.0,
};

// The profile a raster of `px` device pixels is drawn with. Deterministic and
// total: a size of zero or below resolves to Small rather than to no profile.
[[nodiscard]] constexpr const OpticalProfile& OpticalProfileFor(int px) noexcept {
    if (px <= kSmallProfileMaxPx)
        return kSmallProfile;
    if (px <= kMediumProfileMaxPx)
        return kMediumProfile;
    return kLargeProfile;
}

// ---------------------------------------------------------------------------
// Transport glyphs
// ---------------------------------------------------------------------------
// One set of shapes, on the same 32-unit grid, for the thumbnail toolbar and the
// tray menu items. Written down once because a pause bar that is one width in
// the menu and another in the toolbar is two shapes, and the second one drifts.
inline constexpr double kGlyphDiscRadius = 8.0;
inline constexpr double kGlyphSquareHalf = 7.0;
inline constexpr double kGlyphBarWidth = 3.6;
inline constexpr double kGlyphBarHeight = 15.0;
inline constexpr double kGlyphBarGap = 3.4;
inline constexpr double kGlyphTriangleBackX = 11.5;
inline constexpr double kGlyphTriangleTipX = 23.5;
inline constexpr double kGlyphTriangleHalfHeight = 8.0;

} // namespace exosnap::ui::brand
