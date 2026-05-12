#pragma once

#include <cstdint>
#include <functional>

namespace recorder_core {

struct SessionStats {
    uint64_t video_frames_captured        = 0;
    uint64_t encoded_video_packets        = 0;
    uint64_t audio_packets                = 0;
    uint64_t video_bytes                  = 0;
    uint64_t audio_bytes                  = 0;
    uint64_t output_file_bytes            = 0;   // best-effort; 0 if not yet known
    double   elapsed_seconds              = 0.0;
    uint64_t video_duration_ns            = 0;
    uint64_t audio_duration_ns            = 0;
    double   duration_skew_ms             = 0.0;
    uint64_t dropped_or_skipped_video_frames = 0;
    bool     source_loss                  = false;
};

// Callback invoked approximately every 250 ms while recording is active.
// Called from an internal worker thread — implementations must be thread-safe.
using StatsCallback = std::function<void(const SessionStats&)>;

} // namespace recorder_core
