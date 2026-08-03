#include "playback_clock.h"

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

size_t AudioPrerollFramesToDrop(int64_t block_pts_us, size_t block_frame_count, uint32_t sample_rate_hz,
                                int64_t start_us) noexcept {
    if (block_frame_count == 0 || sample_rate_hz == 0)
        return 0;
    if (block_pts_us >= start_us)
        return 0; // block begins at or after the requested start: nothing is preroll

    const int64_t offset_us = start_us - block_pts_us;
    const auto rate = static_cast<int64_t>(sample_rate_hz);
    // Round to the nearest sample rather than truncating: a timestamp landing
    // a microsecond short of a sample boundary must not leave one sample of
    // the past in the ring (nor eat one that belongs to the future).
    const int64_t to_drop = (offset_us * rate + 500'000) / 1'000'000;
    if (to_drop <= 0)
        return 0;
    if (static_cast<uint64_t>(to_drop) >= static_cast<uint64_t>(block_frame_count))
        return block_frame_count; // the whole block is still before the start
    return static_cast<size_t>(to_drop);
}

} // namespace recorder_core
