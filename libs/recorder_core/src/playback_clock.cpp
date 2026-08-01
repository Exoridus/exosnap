#include "playback_clock.h"

#include <algorithm>
#include <cmath>

namespace recorder_core {

int64_t AudioClockMs(uint64_t frames_played, uint32_t sample_rate_hz) noexcept {
    if (sample_rate_hz == 0)
        return 0;
    return static_cast<int64_t>((frames_played * 1000ull) / sample_rate_hz);
}

uint64_t ClockPositionToFrames(uint64_t position, uint64_t frequency, uint32_t target_rate_hz) noexcept {
    if (frequency == 0 || target_rate_hz == 0)
        return 0;
    // Divide first so a long clip cannot overflow position * target_rate_hz
    // (a shared-mode position counts BYTES, so it grows ~192 kB per second).
    const uint64_t whole = position / frequency;
    const uint64_t rem = position % frequency;
    return whole * target_rate_hz + (rem * target_rate_hz) / frequency;
}

uint64_t InterpolateClockPosition(uint64_t position, uint64_t frequency, uint64_t qpc_position_100ns,
                                  uint64_t qpc_now_100ns, uint64_t max_extrapolation_100ns) noexcept {
    if (frequency == 0 || qpc_position_100ns == 0 || qpc_now_100ns <= qpc_position_100ns)
        return position;
    uint64_t delta_100ns = qpc_now_100ns - qpc_position_100ns;
    if (delta_100ns > max_extrapolation_100ns)
        delta_100ns = max_extrapolation_100ns;
    // delta seconds * frequency, in the position's own units. 10^7 100 ns
    // ticks per second; divide first for the same overflow reason as above.
    constexpr uint64_t kTicksPerSecond = 10000000ull;
    const uint64_t advance =
        (delta_100ns / kTicksPerSecond) * frequency + ((delta_100ns % kTicksPerSecond) * frequency) / kTicksPerSecond;
    return position + advance;
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

size_t VideoQueueCapacityForFrameRate(double fps, double decode_ahead_seconds) noexcept {
    // Matches the original hand-computed 60 fps sizing: 60 x 0.2 s = 12 frames
    // in the decode-ahead window, + 1/3 headroom = 16.
    constexpr size_t kFloor = 16;
    if (!(fps > 0.0) || !(decode_ahead_seconds > 0.0))
        return kFloor;
    const double in_window = std::ceil(fps * decode_ahead_seconds);
    if (!(in_window > 0.0) || in_window > 100000.0)
        return kFloor; // absurd declared rate: fall back rather than allocate wildly
    const auto frames = static_cast<size_t>(in_window);
    return std::max(kFloor, frames + frames / 3u);
}

} // namespace recorder_core
