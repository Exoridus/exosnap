#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "models/StableDisplayId.h"

namespace exosnap {

// ---------------------------------------------------------------------------
// PhysicalRect
//
// A monitor rectangle in PHYSICAL virtual-screen pixels — the coordinate space
// of MONITORINFOEXW.rcMonitor and recorder_core::CaptureRegion. Kept as plain
// ints so the pure matcher and region math carry no <windows.h> dependency and
// stay fully unit-testable without real hardware.
//
// NOTE (physical vs. logical): region anchoring MUST use this physical rect, not
// QScreen::geometry() (which is DPI-scaled/logical). Normalizing physical region
// pixels against a logical geometry breaks the round-trip on any scaled display.
// ---------------------------------------------------------------------------
struct PhysicalRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    [[nodiscard]] int width() const noexcept {
        return right - left;
    }
    [[nodiscard]] int height() const noexcept {
        return bottom - top;
    }
    [[nodiscard]] bool valid() const noexcept {
        return right > left && bottom > top;
    }

    friend bool operator==(const PhysicalRect&, const PhysicalRect&) = default;
};

// One enumerated active display, produced by the impure enumerator (Step 2).
struct EnumeratedDisplayIdentity {
    StableDisplayId id;
    uintptr_t hmonitor = 0;           // HMONITOR as an opaque handle value (0 = unknown)
    PhysicalRect rc_monitor_physical; // physical anchor geometry for region math
};

// Ranked match confidence, strongest first.
enum class DisplayMatchConfidence {
    None = 0,     // no match — UNRESOLVED
    GdiName,      // stage 4: degraded identity, last-resort GDI-name match
    FriendlyName, // stage 3: unique {vendor,product} single monitor of a model
    Serial,       // stage 2: same panel {vendor,product,serial} at another port
    DevicePath,   // stage 1: exact monitorDevicePath (same connector)
};

struct DisplayMatch {
    std::size_t index = 0; // index into the enumerated list
    DisplayMatchConfidence confidence = DisplayMatchConfidence::None;
};

// ---------------------------------------------------------------------------
// ResolveStableDisplay
//
// Pure ranked matcher. Returns the best match or nullopt (UNRESOLVED). Never
// guesses among ambiguous twins (same model, no distinct serial). An empty()
// saved id returns nullopt — the caller treats that as "no preference".
//
// Ranking (first hit wins):
//   1. device_path exact                         -> DevicePath
//   2. {edid_vendor, edid_product, serial} exact, serial non-empty
//                                                 -> Serial
//   3. {edid_vendor, edid_product} matches exactly one enumerated display AND
//      friendly_name matches (when saved)         -> FriendlyName
//      (two or more displays of the same model without a serial -> no match)
//   4. gdi_name exact, ONLY when the saved id never carried a device_path
//      (a degraded save) — never worse than the historical GDI-name match, and
//      never a silent mismatch for a rich saved identity  -> GdiName
// ---------------------------------------------------------------------------
[[nodiscard]] std::optional<DisplayMatch>
ResolveStableDisplay(const StableDisplayId& saved, const std::vector<EnumeratedDisplayIdentity>& enumerated);

// ---------------------------------------------------------------------------
// Region normalization (pure)
//
// A capture region is stored relative to its anchor display so a resolution
// change carries the rectangle proportionally instead of leaving it clipped or
// off-screen. Both sides work in PHYSICAL pixels (see PhysicalRect).
// ---------------------------------------------------------------------------
struct NormalizedRegion {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    friend bool operator==(const NormalizedRegion&, const NormalizedRegion&) = default;
};

struct AbsoluteRegion {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    friend bool operator==(const AbsoluteRegion&, const AbsoluteRegion&) = default;
};

// Absolute physical region -> normalized [0,1] fractions of the anchor rect.
// Returns all-zero when the anchor is degenerate.
[[nodiscard]] NormalizedRegion AbsoluteRegionToAnchorRelative(const AbsoluteRegion& region, const PhysicalRect& anchor);

// Normalized fractions -> absolute physical region, clamped inside the anchor.
[[nodiscard]] AbsoluteRegion AnchorRelativeRegionToAbsolute(const NormalizedRegion& norm, const PhysicalRect& anchor);

} // namespace exosnap
