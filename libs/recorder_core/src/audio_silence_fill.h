#pragma once

// Wall-clock silence-fill policy for a capture source that stops delivering.
//
// Two different situations leave an audio worker with no packets to encode:
//
//   1. The endpoint was lost (ADR 0046). The source reports the failure and the
//      worker holds the timeline with silence until it is reacquired.
//   2. The source is perfectly healthy but has nothing to hand over. WASAPI
//      loopback on a render endpoint is the everyday case: while nothing is
//      playing, the audio engine is idle and IAudioCaptureClient delivers no
//      packets at all — not silent ones, none. A track fed only by loopback
//      therefore stops advancing for the whole silent stretch, and every sample
//      captured afterwards is stamped that much too early, because encoder PTS
//      is derived from the accumulated frame counter.
//
// Case 2 is invisible on a merged track that also carries a microphone (the mic
// keeps delivering, so the mixer keeps emitting and the loopback contributes
// zeros), which is why it only shows up on a solo loopback track.
//
// These helpers turn "the source has been quiet for a while" into "inject this
// many frames of silence", as pure arithmetic that can be unit-tested without
// WASAPI.

#include <cstdint>

namespace recorder_core {

// How long a healthy source may deliver nothing before its silence is filled
// from the wall clock. Must comfortably exceed the endpoint buffer period plus
// scheduling jitter so ordinary packet cadence never trips it; a stall is only
// ever noticed when the source reports zero pending frames, so a starved worker
// (which sees a backlog, not an empty queue) cannot trip it either.
inline constexpr uint64_t kSilentStallThresholdNs = 300000000ULL; // 300 ms

// Frames of silence that bring an audio timeline accounted up to
// `last_accounted_ns` level with `now_ns`. Returns 0 when the clock has not
// moved forward. `max_frames` bounds a single call so one long stall never
// allocates or encodes an unbounded block at once — the remainder is filled by
// the following iterations.
inline uint64_t SilenceFillFrames(uint64_t now_ns, uint64_t last_accounted_ns, uint32_t sample_rate,
                                  uint64_t max_frames) noexcept {
    if (sample_rate == 0 || now_ns <= last_accounted_ns) {
        return 0;
    }
    const uint64_t elapsed_ns = now_ns - last_accounted_ns;
    uint64_t frames = (elapsed_ns * sample_rate) / 1000000000ULL;
    if (frames > max_frames) {
        frames = max_frames;
    }
    return frames;
}

// True once a source that is neither paused nor degraded has been quiet long
// enough to be treated as stalled rather than merely between packets.
inline bool IsSilentStall(uint64_t now_ns, uint64_t last_accounted_ns,
                          uint64_t threshold_ns = kSilentStallThresholdNs) noexcept {
    return now_ns > last_accounted_ns && (now_ns - last_accounted_ns) >= threshold_ns;
}

// Frames of a device-reported discontinuity gap that still need filling after
// wall-clock silence already covered part of the same outage. Without this, a
// stall that BOTH the wall clock and the device position report (WASAPI can set
// DATA_DISCONTINUITY with a jumped device position on the first packet after a
// silent stretch) would be filled twice and push the track permanently late.
inline uint32_t RemainingGapFrames(uint32_t gap_frames, uint64_t already_filled_frames) noexcept {
    if (already_filled_frames >= gap_frames) {
        return 0;
    }
    return gap_frames - static_cast<uint32_t>(already_filled_frames);
}

} // namespace recorder_core
