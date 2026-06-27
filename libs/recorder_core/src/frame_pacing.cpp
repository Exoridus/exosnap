#include "recorder_core/frame_pacing.h"
#include <algorithm>

namespace recorder_core {

std::size_t ComputePacingRingSize(uint32_t monitor_refresh_hz, uint32_t output_fps) {
    constexpr std::size_t kMin = 4, kMax = 12, kFallback = 8;
    if (monitor_refresh_hz == 0 || output_fps == 0) {
        return kFallback;
    }
    const std::size_t ratio = (monitor_refresh_hz + output_fps - 1) / output_fps; // ceil
    return std::clamp(ratio + 2, kMin, kMax);
}

} // namespace recorder_core
