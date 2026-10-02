#pragma once

#include <cstdint>
#include <string>

namespace exosnap {

// ---------------------------------------------------------------------------
// StableDisplayId
//
// Persisted connector and panel facts used to restore a display/region target.
// The ranked matcher prefers an exact connector path, even when the panel on
// that connector has changed. This is not proof of physical-panel identity.
// Serial/model fallbacks can resolve a panel at another connector when the
// original path is absent. Runtime capture still keys on the resolved live
// display name rather than this persistence record.
//
// Field sources (see DisplayIdentityEnumerator):
//   device_path   — DISPLAYCONFIG_TARGET_DEVICE_NAME.monitorDevicePath (primary,
//                   disambiguates twins by connector; follows the port)
//   edid_vendor   — PNP manufacturer id, e.g. "GSM"
//   edid_product  — edidProductCodeId
//   serial        — EDID serial number, best-effort; may be empty (follows the
//                   panel across ports when present)
//   friendly_name — monitorFriendlyDeviceName (display + fallback match only)
//   gdi_name      — "\\.\DISPLAYn"; last-resort fallback + debugging
//   seq_hint      — sequential display number at save time (debug/display only)
//
// An empty() id means "no stored preference" (primary/any display).
// ---------------------------------------------------------------------------
struct StableDisplayId {
    std::string device_path;
    std::string edid_vendor;
    uint32_t edid_product = 0;
    std::string serial;
    std::string friendly_name;
    std::string gdi_name;
    int seq_hint = 0;

    // "No stored preference" — no identifying field carries information.
    // seq_hint is debug-only and does not count toward identity.
    [[nodiscard]] bool empty() const noexcept {
        return device_path.empty() && edid_vendor.empty() && edid_product == 0 && serial.empty() &&
               friendly_name.empty() && gdi_name.empty();
    }

    friend bool operator==(const StableDisplayId&, const StableDisplayId&) = default;
};

} // namespace exosnap
