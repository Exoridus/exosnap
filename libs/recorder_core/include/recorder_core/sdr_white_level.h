#pragma once
#include <cmath>

// The overlay reference white used to place SDR sprites (webcam PiP, cursor) on
// the native-HDR10 linear-light timeline. Windows exposes the user's "SDR
// content brightness" per display as DISPLAYCONFIG_SDR_WHITE_LEVEL; DWM renders
// SDR windows at exactly that level, so using it makes the PiP match the
// brightness of SDR content on the same screen. Pure logic only — the
// DisplayConfig query itself lives with the capture backend (impure, needs a
// live HMONITOR).

namespace recorder_core {

// Fallback when the display's SDR white level is unknown: 203 cd/m^2, the
// ITU-R BT.2408 diffuse-white anchor (previous hardcoded behaviour).
inline constexpr float kDefaultSdrWhiteLevelNits = 203.0f;

// Plausibility window for a queried level. The Windows SDR-brightness slider
// spans 80–480 nits; anything outside is a failed or garbage reading.
inline constexpr float kMinPlausibleSdrWhiteNits = 80.0f;
inline constexpr float kMaxPlausibleSdrWhiteNits = 480.0f;

// DISPLAYCONFIG_SDR_WHITE_LEVEL.SDRWhiteLevel is in thousandths of 80 nits
// (1000 == 80 cd/m^2 == scRGB 1.0).
[[nodiscard]] inline float SdrWhiteLevelRawToNits(unsigned long raw) noexcept {
    return static_cast<float>(raw) * 80.0f / 1000.0f;
}

// Maps a queried (or unknown == 0) SDR white level to the overlay reference
// white actually used for compositing.
[[nodiscard]] inline float EffectiveOverlayReferenceWhiteNits(float queried_nits) noexcept {
    if (!std::isfinite(static_cast<double>(queried_nits)) || queried_nits < kMinPlausibleSdrWhiteNits ||
        queried_nits > kMaxPlausibleSdrWhiteNits) {
        return kDefaultSdrWhiteLevelNits;
    }
    return queried_nits;
}

} // namespace recorder_core
