#pragma once

// The display-mode facts a desktop duplication was opened against, and the
// rule for when they have moved from under it -- kept free of DXGI so the rule
// can be unit tested.
//
// IDXGIFactory1::IsCurrent() is documented as an adapter-set signal: it turns
// false when an adapter appears or disappears, which is what a hot-plug does.
// It says nothing about a mode change on an adapter that stays: a refresh-rate
// switch, a resolution change, HDR toggled on or off. Those can leave a
// duplication that never presents again but never reports ACCESS_LOST either,
// so a caller that only asked the factory would wait on it forever. Comparing
// the mode the duplication was opened with against the one the output has now
// is the signal for that class.

#include <cstdint>

namespace exosnap::engine {

struct OutputModeSignature {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t refresh_hz = 0;  // whole Hz; 0 when the mode does not say
    uint32_t orientation = 0; // DEVMODE dmDisplayOrientation
    bool hdr_active = false;  // PQ/BT.2020 colour space on the output
    bool known = false;       // false when the mode could not be read
};

// True when `now` describes a different mode than `opened`. Two readings that
// could not be taken compare as unchanged: an unreadable mode is not evidence
// of a switch, and treating it as one would reopen a healthy duplication on
// every idle acquire.
[[nodiscard]] constexpr bool OutputModeChanged(const OutputModeSignature& opened,
                                               const OutputModeSignature& now) noexcept {
    if (!opened.known || !now.known)
        return false;
    return opened.width != now.width || opened.height != now.height || opened.refresh_hz != now.refresh_hz ||
           opened.orientation != now.orientation || opened.hdr_active != now.hdr_active;
}

} // namespace exosnap::engine
