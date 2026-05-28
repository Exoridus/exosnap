#pragma once

#include <cstdint>
#include <windows.h>

namespace exosnap {

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
