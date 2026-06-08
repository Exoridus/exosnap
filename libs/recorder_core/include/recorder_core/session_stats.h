#pragma once

#include "codec_types.h"
#include "output_geometry.h"

#include <array>
#include <cstdint>
#include <functional>

namespace recorder_core {

struct SessionStats {
    uint64_t video_frames_captured = 0;
    uint64_t encoded_video_packets = 0;
    uint64_t audio_packets = 0;
    uint64_t video_bytes = 0;
    uint64_t audio_bytes = 0;
    uint64_t output_file_bytes = 0; // best-effort; 0 if not yet known
    double elapsed_seconds = 0.0;
    uint64_t video_duration_ns = 0;
    uint64_t audio_duration_ns = 0;
    double duration_skew_ms = 0.0;
    uint64_t dropped_or_skipped_video_frames = 0;
    uint64_t duplicated_video_frames = 0;
    FrameSize source_size;
    FrameSize output_size;
    ContentRect content_rect;
    uint32_t frame_rate_num = 60;
    uint32_t frame_rate_den = 1;
    bool cfr = true;
    Container container = Container::WebM;
    VideoCodec video_codec = VideoCodec::Av1Nvenc;
    AudioCodec audio_codec = AudioCodec::Opus;
    // Smoothed linear RMS level [0..1] per audio track.
    // Index is AudioThread track_id_ and is bounded by CodecPrivateData::kMaxAudioTracks.
    std::array<float, 3> per_track_rms{};
    bool source_loss = false;
};

// Lightweight RMS snapshot for high-cadence meter updates (~30 Hz).
struct MeterSnapshot {
    std::array<float, 3> per_track_rms{};
};

// Callback invoked approximately every 264 ms while recording is active.
// Called from an internal worker thread — implementations must be thread-safe.
using StatsCallback = std::function<void(const SessionStats&)>;

// Callback invoked approximately every 33 ms while recording is active.
// Called from an internal worker thread — implementations must be thread-safe.
using MeterCallback = std::function<void(const MeterSnapshot&)>;

} // namespace recorder_core
