#pragma once

#include <cstdint>
#include <string>

namespace exosnap {

// ---------------------------------------------------------------------------
// StableDisplayId
//
// A hardware-stable identity for a physical display, persisted alongside a
// capture target so a saved monitor/region selection survives topology changes
// (unplug/replug, driver restart, reboot in a different port order) instead of
// riding on the unstable GDI device name ("\\.\DISPLAYn").
//
// This is a PERSISTENCE/RESTORE concern only. The runtime capture machinery
// (hub keying, preview, HDR) still keys on the live GDI device name and is not
// affected. At restore time a ranked matcher (DisplayIdentityResolver) turns a
// saved StableDisplayId back into a concrete current display.
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
