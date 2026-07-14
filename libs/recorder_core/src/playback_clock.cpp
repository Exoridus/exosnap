#include "playback_clock.h"

#include <algorithm>

namespace recorder_core {

int64_t AudioClockMs(uint64_t frames_rendered, uint32_t sample_rate_hz) noexcept {
    if (sample_rate_hz == 0)
        return 0;
    return static_cast<int64_t>((frames_rendered * 1000ull) / sample_rate_hz);
}

FrameSelection SelectFrameForClock(std::span<const int64_t> available_pts_ms, int64_t clock_ms) noexcept {
    FrameSelection sel;
    if (available_pts_ms.empty())
        return sel;

    // upper_bound: first element strictly greater than clock_ms. The frame to
    // show is the one just before it (the latest <= clock_ms).
    const auto it = std::upper_bound(available_pts_ms.begin(), available_pts_ms.end(), clock_ms);
    if (it == available_pts_ms.begin())
        return sel; // clock is before the first frame -- nothing to show yet

    const size_t selected_index = static_cast<size_t>(std::distance(available_pts_ms.begin(), it)) - 1u;
    sel.index = selected_index;
    sel.dropped_count = selected_index; // every frame strictly before it is now stale
    return sel;
}

} // namespace recorder_core
