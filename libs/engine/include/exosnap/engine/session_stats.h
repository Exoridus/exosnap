#pragma once

#include "codec_types.h"
#include "output_geometry.h"

#include <array>
#include <cstdint>
#include <functional>

namespace exosnap::engine {

struct SessionStats {
    uint64_t video_frames_captured = 0;
    uint64_t encoded_video_packets = 0;
    uint64_t audio_packets = 0;
    uint64_t video_bytes = 0;
    uint64_t audio_bytes = 0;
    uint64_t output_file_bytes = 0; // best-effort; 0 if not yet known
    double elapsed_seconds = 0.0;
    uint64_t video_duration_ns = 0;
    // The audio encoder's own last PTS, on the AUDIO capture timeline. Kept
    // because it is the honest raw figure for diagnosing the audio path, and
    // deliberately NOT comparable with video_duration_ns: the muxer shifts every
    // audio track onto the video timeline before writing it, so this differs from
    // what the file contains by exactly that shift.
    uint64_t audio_duration_ns = 0;
    // The last audio end the MUXER actually wrote, on the same aligned timeline
    // video_duration_ns is on, per track. This is the figure the file's own
    // timestamps agree with, and the only one duration_skew_ms may be computed
    // from. Zero for a track that had nothing written.
    std::array<uint64_t, 3> aligned_audio_duration_ns{};
    // |video end - aligned audio end| on one timeline. Comparing video against
    // the unshifted audio_duration_ns reported the epoch offset as skew: a
    // correctly aligned recording whose audio clock started 200 ms after the
    // video clock read as 200 ms of drift.
    //
    // Not an A/V sync proof. It compares where the two streams END; two streams
    // that end together can still have drifted apart and back in between.
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
    VideoCodec video_codec = VideoCodec::Av1;
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
    // The video encoder's end-of-stream drain was cut short: EOS refused, the
    // device stopped delivering within the drain budget, or a lock failed. The
    // file was still finalised with every packet that did drain, so this is a
    // post-flight fact about the tail of the recording -- `undrained` frames
    // were submitted and are not in the file. Written once at end of stream.
    bool video_flush_incomplete = false;
    uint64_t video_undrained_frames = 0;
    // Audio packets that arrived for a segment the muxer had already finalized
    // at a split boundary. They are not in any file: the segment they belong to
    // is closed, and writing them into the next one at its epoch would misplace
    // them rather than keep them. Non-zero means the split cost this much audio.
    uint64_t audio_packets_trimmed_at_split = 0;
};

// The A/V duration skew, from the two ends that are on the same timeline.
//
// One function so the live snapshot and the end-of-session summary cannot drift
// apart -- they had the same computation written twice, against the wrong audio
// figure both times.
//
// The aligned audio end is what the MUXER wrote; `audio_duration_ns` is the
// encoder's own last PTS on the capture timeline and differs from it by the
// track's alignment shift. Comparing that against the video end reported a
// correctly aligned recording as skewed by exactly its epoch offset.
//
// Unavailable rather than 0 when either end is missing: "no skew" and "nothing
// to compare" are different, and a caller that cannot tell them apart publishes
// a zero it did not measure. Multi-track sessions take the worst track -- the
// question is whether ANY audio ends away from the picture.
struct DurationSkew {
    bool available = false;
    double ms = 0.0;
};

[[nodiscard]] inline DurationSkew
ComputeDurationSkew(uint64_t video_duration_ns, const std::array<uint64_t, 3>& aligned_audio_duration_ns) noexcept {
    if (video_duration_ns == 0)
        return {};
    DurationSkew worst;
    for (const uint64_t audio_end : aligned_audio_duration_ns) {
        if (audio_end == 0)
            continue; // this track had nothing written; it is not a zero skew
        const double vd = static_cast<double>(video_duration_ns) / 1e6;
        const double ad = static_cast<double>(audio_end) / 1e6;
        const double skew = vd > ad ? vd - ad : ad - vd;
        if (!worst.available || skew > worst.ms) {
            worst.available = true;
            worst.ms = skew;
        }
    }
    return worst;
}

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

} // namespace exosnap::engine
