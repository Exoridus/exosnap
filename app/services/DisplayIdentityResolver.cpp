#include "services/DisplayIdentityResolver.h"

#include <algorithm>
#include <cmath>

namespace exosnap {

namespace {

// Same physical panel/model key (vendor + product code). Both non-empty/non-zero.
bool SameModel(const StableDisplayId& a, const StableDisplayId& b) {
    return !a.edid_vendor.empty() && a.edid_vendor == b.edid_vendor && a.edid_product != 0 &&
           a.edid_product == b.edid_product;
}

float ClampUnit(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

int RoundToInt(float v) {
    return static_cast<int>(std::lround(v));
}

} // namespace

std::optional<DisplayMatch> ResolveStableDisplay(const StableDisplayId& saved,
                                                 const std::vector<EnumeratedDisplayIdentity>& enumerated) {
    if (saved.empty()) {
        return std::nullopt; // no stored preference
    }

    // ---- Stage 1: exact device path (same connector) ----
    if (!saved.device_path.empty()) {
        for (std::size_t i = 0; i < enumerated.size(); ++i) {
            if (enumerated[i].id.device_path == saved.device_path) {
                return DisplayMatch{i, DisplayMatchConfidence::DevicePath};
            }
        }
    }

    // ---- Stage 2: same panel via EDID serial (moved to another port) ----
    if (!saved.serial.empty() && SameModel(saved, saved)) {
        for (std::size_t i = 0; i < enumerated.size(); ++i) {
            const StableDisplayId& e = enumerated[i].id;
            if (SameModel(saved, e) && !e.serial.empty() && e.serial == saved.serial) {
                return DisplayMatch{i, DisplayMatchConfidence::Serial};
            }
        }
    }

    // ---- Stage 3: unique single monitor of the same model ----
    if (!saved.edid_vendor.empty() && saved.edid_product != 0) {
        std::size_t candidate = 0;
        int candidate_count = 0;
        for (std::size_t i = 0; i < enumerated.size(); ++i) {
            if (SameModel(saved, enumerated[i].id)) {
                candidate = i;
                ++candidate_count;
            }
        }
        // Exactly one display of this model system-wide -> unambiguous. Two or
        // more (twins without a distinguishing serial) -> do NOT guess.
        if (candidate_count == 1) {
            const StableDisplayId& e = enumerated[candidate].id;
            const bool friendly_ok = saved.friendly_name.empty() || e.friendly_name == saved.friendly_name;
            if (friendly_ok) {
                return DisplayMatch{candidate, DisplayMatchConfidence::FriendlyName};
            }
        }
    }

    // ---- Stage 4: degraded identity — last-resort GDI-name match ----
    // Only when the saved id never carried a device_path. A rich saved identity
    // whose device_path did not match must stay UNRESOLVED rather than risk a
    // silent mismatch to a different physical monitor.
    if (saved.device_path.empty() && !saved.gdi_name.empty()) {
        for (std::size_t i = 0; i < enumerated.size(); ++i) {
            if (enumerated[i].id.gdi_name == saved.gdi_name) {
                return DisplayMatch{i, DisplayMatchConfidence::GdiName};
            }
        }
    }

    return std::nullopt; // UNRESOLVED
}

NormalizedRegion AbsoluteRegionToAnchorRelative(const AbsoluteRegion& region, const PhysicalRect& anchor) {
    NormalizedRegion norm;
    if (!anchor.valid()) {
        return norm; // all-zero; degenerate anchor
    }
    const float w = static_cast<float>(anchor.width());
    const float h = static_cast<float>(anchor.height());
    norm.x = ClampUnit(static_cast<float>(region.x - anchor.left) / w);
    norm.y = ClampUnit(static_cast<float>(region.y - anchor.top) / h);
    norm.w = ClampUnit(static_cast<float>(region.width) / w);
    norm.h = ClampUnit(static_cast<float>(region.height) / h);
    return norm;
}

AbsoluteRegion AnchorRelativeRegionToAbsolute(const NormalizedRegion& norm, const PhysicalRect& anchor) {
    AbsoluteRegion out;
    if (!anchor.valid()) {
        return out; // all-zero; degenerate anchor
    }
    const float w = static_cast<float>(anchor.width());
    const float h = static_cast<float>(anchor.height());

    const float nx = ClampUnit(norm.x);
    const float ny = ClampUnit(norm.y);
    const float nw = ClampUnit(norm.w);
    const float nh = ClampUnit(norm.h);

    out.x = anchor.left + RoundToInt(nx * w);
    out.y = anchor.top + RoundToInt(ny * h);
    out.width = RoundToInt(nw * w);
    out.height = RoundToInt(nh * h);

    // Clamp the rectangle fully inside the anchor.
    if (out.x < anchor.left) {
        out.x = anchor.left;
    }
    if (out.y < anchor.top) {
        out.y = anchor.top;
    }
    if (out.width < 0) {
        out.width = 0;
    }
    if (out.height < 0) {
        out.height = 0;
    }
    if (out.x + out.width > anchor.right) {
        out.width = anchor.right - out.x;
    }
    if (out.y + out.height > anchor.bottom) {
        out.height = anchor.bottom - out.y;
    }
    return out;
}

} // namespace exosnap
