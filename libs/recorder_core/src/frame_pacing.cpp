#include "recorder_core/frame_pacing.h"
#include <algorithm>
#include <cstdint>
#include <span>

namespace recorder_core {

std::size_t ComputePacingRingSize(uint32_t monitor_refresh_hz, uint32_t output_fps) {
    constexpr std::size_t kMin = 4, kMax = 12, kFallback = 8;
    if (monitor_refresh_hz == 0 || output_fps == 0) {
        return kFallback;
    }
    const std::size_t ratio = (monitor_refresh_hz + output_fps - 1) / output_fps; // ceil
    return std::clamp(ratio + 2, kMin, kMax);
}

PacingDecision SelectFrameForSlot(std::span<const uint64_t> ring_present_qpc, uint64_t slot_qpc,
                                  uint64_t last_emitted_present_qpc, FramePacingMode mode) {
    PacingDecision d;
    // Find the fresh window: entries strictly newer than the last emitted present time.
    std::size_t first_fresh = ring_present_qpc.size();
    for (std::size_t i = 0; i < ring_present_qpc.size(); ++i) {
        if (ring_present_qpc[i] > last_emitted_present_qpc) {
            first_fresh = i;
            break;
        }
    }
    if (first_fresh == ring_present_qpc.size()) {
        return d; // no fresh entry → duplicate
    }
    std::size_t chosen = first_fresh;
    if (mode == FramePacingMode::Newest) {
        chosen = ring_present_qpc.size() - 1; // last (newest) fresh entry
    } else {
        // Smooth: nearest present time to slot among fresh entries (ascending → first min wins on tie).
        uint64_t best_dist = UINT64_MAX;
        for (std::size_t i = first_fresh; i < ring_present_qpc.size(); ++i) {
            const uint64_t p = ring_present_qpc[i];
            const uint64_t dist = p >= slot_qpc ? p - slot_qpc : slot_qpc - p;
            if (dist < best_dist) {
                best_dist = dist;
                chosen = i;
            }
        }
    }
    d.emit = true;
    d.index = chosen;
    d.newly_dropped = static_cast<uint32_t>(chosen - first_fresh); // fresh entries older than chosen, skipped
    return d;
}

} // namespace recorder_core
