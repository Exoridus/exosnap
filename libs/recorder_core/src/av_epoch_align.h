#pragma once

// A/V epoch alignment: where the audio timeline starts relative to the video
// timeline.
//
// Video PTS 0 is the instant the video epoch was taken; audio PTS 0 is the
// instant the first captured audio sample was recorded at. Both are QPC
// readings (100 ns units), and they are NOT the same instant — the two workers
// open different devices, at different costs, after the session clock started.
// The muxer therefore has to shift the audio track by their difference before
// writing it next to video; assuming the audio started when Record() was called
// makes the whole track lead video by however long the audio device took to
// come up.
//
// A positive shift means audio starts after video (the track is written with a
// silent head); a negative shift means audio starts before video (the head is
// trimmed).

#include <cstdint>

namespace recorder_core {

// Largest audio/video epoch difference that is treated as a real measurement.
// The two workers are started by the same Record() call, so a difference beyond
// this is a bad clock reading, not a real offset — clamping keeps a bogus
// timestamp from pushing the audio track seconds away from the picture.
inline constexpr int64_t kMaxAudioEpochShiftNs = 5000000000LL; // 5 s

// Where audio PTS 0 sits on the wall clock, given a capture packet's QPC
// timestamp and how many frames the encoder timeline already held in front of
// it. `frames_on_timeline` must be the count actually FED to the encoder ahead
// of this packet -- counting silence that was decided against (a gap the wall
// clock had already covered) walks the epoch back over time the track does not
// contain and lands it that much too early. Returns 0 ("not measurable") when
// the walk-back would run before the QPC origin.
inline uint64_t AudioEpochNsFromPacket(uint64_t packet_qpc_ns, uint64_t frames_on_timeline,
                                       uint32_t sample_rate) noexcept {
    if (sample_rate == 0) {
        return 0;
    }
    const uint64_t elapsed_ns = (frames_on_timeline * 1000000000ULL) / sample_rate;
    if (packet_qpc_ns <= elapsed_ns) {
        return 0;
    }
    return packet_qpc_ns - elapsed_ns;
}

// Whether a measured audio epoch places the track within a believable distance
// of the video epoch. A reading outside this is a bad clock, not a real offset:
// the caller should discard the measurement and fall back to the session
// baseline rather than apply a clamped -- but still wrong by seconds -- shift.
inline bool IsPlausibleAudioEpoch(uint64_t audio_epoch_100ns, uint64_t video_epoch_100ns,
                                  int64_t max_abs_ns = kMaxAudioEpochShiftNs) noexcept {
    if (audio_epoch_100ns == 0) {
        return false; // never measured
    }
    const int64_t shift = (static_cast<int64_t>(audio_epoch_100ns) - static_cast<int64_t>(video_epoch_100ns)) * 100LL;
    return shift <= max_abs_ns && shift >= -max_abs_ns;
}

// Nanoseconds to add to an audio packet's session PTS to place it on the video
// timeline. Both epochs are QPC readings in 100 ns units. The clamp is a
// last-resort guard only -- callers should reject an implausible measurement
// via IsPlausibleAudioEpoch first, so a bad reading falls back to the session
// baseline instead of being applied as a full-magnitude shift.
inline int64_t AudioTimelineShiftNs(uint64_t audio_epoch_100ns, uint64_t video_epoch_100ns,
                                    int64_t max_abs_ns = kMaxAudioEpochShiftNs) noexcept {
    const int64_t shift = (static_cast<int64_t>(audio_epoch_100ns) - static_cast<int64_t>(video_epoch_100ns)) * 100LL;
    if (shift > max_abs_ns) {
        return max_abs_ns;
    }
    if (shift < -max_abs_ns) {
        return -max_abs_ns;
    }
    return shift;
}

// Apply a shift to a session PTS. Returns false when the packet falls before
// the video epoch entirely and must be dropped rather than written at 0.
inline bool ShiftAudioPts(uint64_t pts_ns, int64_t shift_ns, uint64_t& out_pts_ns) noexcept {
    const int64_t shifted = static_cast<int64_t>(pts_ns) + shift_ns;
    if (shifted < 0) {
        return false;
    }
    out_pts_ns = static_cast<uint64_t>(shifted);
    return true;
}

} // namespace recorder_core
