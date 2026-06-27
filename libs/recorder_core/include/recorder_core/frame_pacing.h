#pragma once
#include <cstdint>

namespace recorder_core {

// CFR output pacing. Smooth = phase-correct present-time-nearest selection (default,
// the recording use case). Newest = lowest-latency newest-at-tick (and the WGC fallback).
enum class FramePacingMode : uint8_t {
    Smooth = 0,
    Newest = 1,
};

} // namespace recorder_core
