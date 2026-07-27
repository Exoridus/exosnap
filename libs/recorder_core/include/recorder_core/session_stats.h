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
    // Resampler tail flushed at stop, per audio track (same indexing as
    // per_track_rms). `drained` counts the frames libswresample still held in its
    // filter delay and that the drain pushed into the encoder; `undrained` is what
    // the flush loop left behind at its iteration bound (expected 0 — a non-zero
    // value means that much captured audio never reached the file). Written once
    // per track at end of stream, so a session report reads a final figure.
    // The drain only runs on a track that reached end of stream cleanly, so a
    // failed or timed-out session leaves the two counters below at their initial
    // 0 — which is not a measurement of "nothing was left behind". This bit says
    // whether the drain actually ran for that track; without it the counters mean
    // nothing and must not be reported as a figure.
    std::array<bool, 3> per_track_resampler_drain_recorded{};
    std::array<uint64_t, 3> per_track_resampler_drained_frames{};
    std::array<uint64_t, 3> per_track_resampler_undrained_frames{};
    bool source_loss = false;
    // True once any audio capture source was lost mid-recording and degraded to
    // honest silence (ADR 0046). A post-flight fact so the "Saved" report can
    // note the recording contains a silence gap, rather than surprising the user.
    bool audio_degraded_occurred = false;
    // Set once when a requested webcam PiP / cursor overlay cannot be recorded in
    // the active mode (native HDR10 from an already-PQ 10-bit desktop composites
    // nothing — the surface is non-linear). Surfaced as a calm diagnostics notice,
    // never a blocker.
    bool webcam_overlay_omitted = false;
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
