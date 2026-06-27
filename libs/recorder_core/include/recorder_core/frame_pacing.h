#pragma once
#include <cstddef>
#include <cstdint>

namespace recorder_core {

// CFR output pacing. Smooth = phase-correct present-time-nearest selection (default,
// the recording use case). Newest = lowest-latency newest-at-tick (and the WGC fallback).
enum class FramePacingMode : uint8_t {
    Smooth = 0,
    Newest = 1,
};

// Ring size for phase-correct pacing: clamp(ceil(refresh/fps)+2, 4, 12); 8 when refresh
// or fps is unknown (0). Sized for the source-faster-than-output case (e.g. 240->60).
[[nodiscard]] std::size_t ComputePacingRingSize(uint32_t monitor_refresh_hz, uint32_t output_fps);

} // namespace recorder_core
