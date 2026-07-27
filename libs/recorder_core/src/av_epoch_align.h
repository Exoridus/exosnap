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

// Nanoseconds to add to an audio packet's session PTS to place it on the video
// timeline. Both epochs are QPC readings in 100 ns units.
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
