#pragma once

// What the brand marks are made of, apart from their shapes.
//
// SOURCE OF TRUTH
// ---------------
// The SHAPES are not here. They live in `app/assets/brand/marks/*.svg`, written
// by `scripts/generate-brand-marks.py` from `marks/parameters.json`, which is the
// only place the aperture's five numbers exist. Moving a radius is an edit to
// that file and a re-run of that script; nothing in C++, in QML or in a second
// generator restates a coordinate.
//
// What IS here is everything the SVG cannot carry:
//
//   * the reference colours the suite is authored in, and therefore the exact
//     strings the runtime substitutes the running theme's colours for;
//   * the optical corrections a 16 px raster needs and a vector does not;
//   * the transport glyphs, which are shell chrome rather than brand marks and
//     have no designer composition to derive from.
//
// `brand_geometry_tests` is the drift guard: it regenerates the suite, checks
// every colour in it against the table below, and fails if a checked-in asset
// stopped matching its parameters.

#include <QtGlobal>

namespace exosnap::ui::brand {

// ---------------------------------------------------------------------------
// The design grid
// ---------------------------------------------------------------------------
// The suite's viewBox. Callers need it to place a mark inside a larger canvas;
// no radius is derived from it here.
inline constexpr double kGrid = 32.0;
inline constexpr double kCenter = 16.0;

// ---------------------------------------------------------------------------
// The reference palette
// ---------------------------------------------------------------------------
// The literal colours `scripts/generate-brand-marks.py` writes into the suite,
// and the roles they stand for. The runtime replaces each with the resolved
// colour of the running appearance and accent, so a shipped mark carries the
// user's palette rather than the designer's.
//
// These are exact strings on purpose: a substitution that has to parse colours
// would silently pass an unknown one through, and an unrecoloured mark in the
// notification area is a defect nothing else would catch. The validation test
// asserts every colour in every asset is one of these four.
inline constexpr char kReferenceAccent[] = "#9BD9D2";
inline constexpr char kReferenceRecording[] = "#E0786C";
inline constexpr char kReferenceCaution[] = "#E7C875";
inline constexpr char kReferenceSuccess[] = "#8FD0AF";

// The outer ring's opacity, likewise as the literal the suite carries. The suite
// is written at the dark value; the light appearance needs a heavier ring to
// hold its own against a near-white ground, and that difference is the only one
// between the two themes' marks -- which is why they are one set of files and a
// substitution rather than two.
inline constexpr double kReferenceOuterOpacity = 0.64;
inline constexpr double kOuterOpacityDark = 0.64;
inline constexpr double kOuterOpacityLight = 0.82;

// ---------------------------------------------------------------------------
// Optical sizing
// ---------------------------------------------------------------------------
// The same drawing, corrected for the size it is rasterized at. Not three logos:
// every profile renders the identical suite, and only the weight of what is
// drawn and the margin around it move.
//
// The correction exists because thin strokes do not survive rasterization. At
// 16 px the outer ring is 1.75/32 * 16 = 0.875 device pixels; antialiased into
// less than a pixel it turns into a grey suggestion of a circle, and the margin
// a large icon needs costs whole pixels the mark needs more.
//
// The values were chosen from rendered evidence at each size, not derived. The
// evidence is reproducible: set EXOSNAP_SHELL_ICON_EVIDENCE_DIR and run
// `shell_icon_renderer_tests`, which writes the whole suite at every shell size
// for both appearances.
//
// WHAT THE CORRECTION DOES NOT FIX. At 16 px the aperture leaves roughly six
// device pixels inside the inner ring, and a pause bar or a dashed arc drawn in
// them is under a pixel wide whatever it is multiplied by. At that size the
// COLOUR carries the state -- coral recording, amber paused, green saved -- and
// the glyph starts to read from 20 px up. Making it read at 16 would mean a
// second, simpler composition, which is a different mark rather than a
// correction to this one.
struct OpticalProfile {
    // Multiplies every stroke, and the width of every upright bar -- the pause
    // and processing glyphs are fills rather than strokes, and a correction that
    // reached only the rings would leave them the one thin thing left.
    double stroke_scale;
    // Multiplies the outer ring's opacity. A ring that is thin AND pale is the
    // first thing to disappear.
    double outer_opacity_scale;
    // Multiplies the standalone margin. A small icon can afford less of it,
    // because at 16 px the margin costs whole pixels the mark needs more.
    double content_scale;
};

// Where the profiles change over. Small covers the notification area and the
// Explorer list at 100 % DPI; Medium covers the ordinary shell sizes and the
// scaled small ones; Large is the designer cut unmodified.
inline constexpr int kSmallProfileMaxPx = 20;
inline constexpr int kMediumProfileMaxPx = 48;

inline constexpr OpticalProfile kSmallProfile{
    .stroke_scale = 1.12,
    .outer_opacity_scale = 1.30,
    .content_scale = 1.12,
};

inline constexpr OpticalProfile kMediumProfile{
    .stroke_scale = 1.08,
    .outer_opacity_scale = 1.12,
    .content_scale = 1.06,
};

inline constexpr OpticalProfile kLargeProfile{
    .stroke_scale = 1.0,
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

// Inline in the UI the mark fills its box edge to edge. As a standalone icon it
// needs margin: Windows draws a tray icon hard against its neighbours and an
// application icon against the taskbar's own edges, and a full-bleed ring reads
// as a crop rather than as a circle.
inline constexpr double kStandaloneContentScale = 0.88;

// ---------------------------------------------------------------------------
// Transport glyphs
// ---------------------------------------------------------------------------
// One set of shapes, on the same 32-unit grid, for the thumbnail toolbar and the
// tray menu items. They are not brand marks -- they are the shell's own record,
// pause, resume and stop -- and the designer suite has no composition for them,
// so this is where they live. Written down once because a pause bar that is one
// width in the menu and another in the toolbar is two shapes, and the second one
// drifts.
inline constexpr double kGlyphDiscRadius = 8.0;
inline constexpr double kGlyphSquareHalf = 7.0;
inline constexpr double kGlyphBarWidth = 3.6;
inline constexpr double kGlyphBarHeight = 15.0;
inline constexpr double kGlyphBarGap = 3.4;
inline constexpr double kGlyphTriangleBackX = 11.5;
inline constexpr double kGlyphTriangleTipX = 23.5;
inline constexpr double kGlyphTriangleHalfHeight = 8.0;

} // namespace exosnap::ui::brand
