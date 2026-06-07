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
