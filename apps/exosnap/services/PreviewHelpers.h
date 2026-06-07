#pragma once

#include <cstdint>
#include <optional>
#include <windows.h>

namespace exosnap {

// ---------------------------------------------------------------------------
// PreviewCropBox
// ---------------------------------------------------------------------------

// Crop rectangle in monitor-relative physical pixels.
// x=0, y=0 is the top-left corner of the monitor frame as captured by WGC.
// This is separate from virtual-screen coordinates: the monitor origin is
// subtracted before filling this struct (see RegionToCropBox).
struct PreviewCropBox {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;

    // A crop box is valid when it has positive dimensions and a non-negative
    // top-left offset (i.e., the region starts at or within the monitor frame).
    [[nodiscard]] bool IsValid() const noexcept {
        return x >= 0 && y >= 0 && width > 0 && height > 0;
    }
};

// ---------------------------------------------------------------------------
// RegionToCropBox
// ---------------------------------------------------------------------------

// Convert a virtual-screen CaptureRegion to a monitor-relative crop box.
//
// Coordinate spaces:
//   region_x/y:          virtual-screen physical pixels (CaptureRegion coords).
//   monitor_origin_x/y:  virtual-screen physical pixels (rcMonitor.left / rcMonitor.top
//                         from GetMonitorInfoW — the monitor's top-left in the global
//                         virtual screen).
//
// The WGC captured frame for a monitor starts at pixel (0,0) regardless of the
// monitor's virtual-screen position.  Subtracting the monitor origin converts a
// virtual-screen point to a frame-relative offset.
//
// Returns a PreviewCropBox whose IsValid() is false when the resulting offset is
// negative (the region extends before the monitor's top-left corner).
inline PreviewCropBox RegionToCropBox(int32_t region_x, int32_t region_y, int32_t region_width, int32_t region_height,
                                      int32_t monitor_origin_x, int32_t monitor_origin_y) noexcept {
    PreviewCropBox box;
    // Subtract the monitor origin to go from virtual-screen space to monitor-frame space.
    box.x = region_x - monitor_origin_x;
    box.y = region_y - monitor_origin_y;
    box.width = region_width;
    box.height = region_height;
    return box;
}

// ---------------------------------------------------------------------------
// PreviewCropBox equality
// ---------------------------------------------------------------------------

[[nodiscard]] inline bool operator==(const PreviewCropBox& a, const PreviewCropBox& b) noexcept {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

// ---------------------------------------------------------------------------
// PreviewConfigKey — change-detection key for preview restart decisions.
//
// Captures the essential identity of a preview request: which target (by index
// and OS handle), its kind, and whether a Region crop is active.  The region
// coordinates are stored in virtual-screen space so the key can be compared
// without re-calling QueryScreenPresentation.
//
// A default-constructed key is "no active preview" (target_index == -1).
// ---------------------------------------------------------------------------

struct PreviewConfigKey {
    int32_t target_index = -1; // index in view_model_.targets; -1 = no preview
    intptr_t native_id = 0;    // CaptureTarget::native_id (OS handle / HMONITOR / HWND)
    int32_t kind = 0;          // 0 = Monitor, 1 = Window (matches CaptureTarget::Kind)
    bool has_crop = false;
    // Virtual-screen physical-pixel coordinates of the region (only valid when has_crop=true).
    int32_t region_x = 0;
    int32_t region_y = 0;
    int32_t region_w = 0;
    int32_t region_h = 0;

    [[nodiscard]] bool IsValid() const noexcept {
        return target_index >= 0;
    }

    [[nodiscard]] bool operator==(const PreviewConfigKey& o) const noexcept {
        if (target_index != o.target_index)
            return false;
        if (native_id != o.native_id)
            return false;
        if (kind != o.kind)
            return false;
        if (has_crop != o.has_crop)
            return false;
        if (has_crop) {
            if (region_x != o.region_x || region_y != o.region_y)
                return false;
            if (region_w != o.region_w || region_h != o.region_h)
                return false;
        }
        return true;
    }
};

// Returns true when any aspect of the preview configuration changed and a
// renderer restart is required.  The `active` key describes the last
// successfully started preview; a default-constructed key means no preview
// is active.
[[nodiscard]] inline bool NeedsPreviewRestart(const PreviewConfigKey& requested,
                                              const PreviewConfigKey& active) noexcept {
    return !(requested == active);
}

// ---------------------------------------------------------------------------
// PreviewFrameIntervalMs
// ---------------------------------------------------------------------------

inline uint32_t PreviewFrameIntervalMs(uint32_t frame_rate_num, uint32_t frame_rate_den) {
    if (frame_rate_num == 0 || frame_rate_den == 0)
        return 16;
    const double fps = static_cast<double>(frame_rate_num) / static_cast<double>(frame_rate_den);
    if (fps <= 0.0)
        return 16;
    return static_cast<uint32_t>(1000.0 / fps);
}

inline void ComputeContainFitRect(LONG backbufferW, LONG backbufferH, LONG srcW, LONG srcH, LONG& outX, LONG& outY,
                                  LONG& outW, LONG& outH) {
    if (srcW <= 0 || srcH <= 0) {
        outX = 0;
        outY = 0;
        outW = backbufferW;
        outH = backbufferH;
        return;
    }

    const double scaleX = static_cast<double>(backbufferW) / static_cast<double>(srcW);
    const double scaleY = static_cast<double>(backbufferH) / static_cast<double>(srcH);
    const double scale = (scaleX < scaleY) ? scaleX : scaleY;

    outW = static_cast<LONG>(static_cast<double>(srcW) * scale);
    outH = static_cast<LONG>(static_cast<double>(srcH) * scale);
    outX = (backbufferW - outW) / 2;
    outY = (backbufferH - outH) / 2;
}

} // namespace exosnap
