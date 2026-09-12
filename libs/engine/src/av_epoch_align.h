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

namespace exosnap::engine {

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

// The VFR video epoch: the instant video PTS 0 is derived from. The first
// captured frame can carry a present timestamp from BEFORE recording began — a
// desktop that sat static for seconds reports its last real present — and
// deriving PTS from that origin inflates every later frame's PTS by the idle
// gap, stretching the file beyond the real recording time. Clamp the origin to
// the session start so the timeline never begins before the recording did. A
// non-positive frame timestamp is a bad clock reading and falls back to the
// session start alone.
inline int64_t ClampedVfrVideoEpochTicks100ns(int64_t first_frame_ticks_100ns,
                                              uint64_t session_start_qpc_100ns) noexcept {
    const int64_t session_start = session_start_qpc_100ns > static_cast<uint64_t>(INT64_MAX)
                                      ? INT64_MAX
                                      : static_cast<int64_t>(session_start_qpc_100ns);
    if (first_frame_ticks_100ns <= session_start) {
        return session_start;
    }
    return first_frame_ticks_100ns;
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

// ---------------------------------------------------------------------------
// Placing an aligned packet on a SEGMENT's local timeline.
//
// A Matroska segment's timestamps start at 0 at its own epoch: the first video
// packet written into it. Rebasing a session PTS onto that was
//
//     epoch unset      -> write the session PTS unchanged
//     before the epoch -> write 0
//
// and both are wrong once a split has happened. After a split at ten minutes
// the next segment's epoch is not yet known when the first audio arrives -- the
// queues are independent, and the forced keyframe that sets the epoch may still
// be behind it -- so that audio was written ten minutes into a file that is
// seconds long. Audio that arrives after the epoch is set but belongs before it
// was written at 0 instead, stacking every such packet on the same timestamp.
//
// The packet's own timeline position is the only thing that can decide this, so
// it is decided explicitly and each outcome is named. There is no "write it
// somewhere and hope": a packet that belongs to a segment already closed is
// trimmed, and trimmed packets are counted, because silently dropping audio and
// silently misplacing it are both defects and only one of them is visible.
// ---------------------------------------------------------------------------
enum class SegmentPlacement {
    Write, // On this segment's timeline, at the returned local PTS.
    Defer, // The segment's epoch is not known yet: hold the packet, do not guess.
    Trim,  // Belongs before this segment began. The previous segment is closed.
};

struct SegmentLocalPts {
    SegmentPlacement placement = SegmentPlacement::Defer;
    uint64_t local_pts_ns = 0;
};

// `epoch_set` and `epoch_session_pts_ns` describe the segment; `pts_ns` is the
// packet's session PTS, already shifted onto the video timeline for audio.
//
// A packet exactly at the epoch is written at 0 -- that is the epoch's own
// definition, not a clamp.
[[nodiscard]] inline SegmentLocalPts PlaceOnSegmentTimeline(uint64_t pts_ns, bool epoch_set,
                                                            uint64_t epoch_session_pts_ns) noexcept {
    if (!epoch_set)
        return SegmentLocalPts{SegmentPlacement::Defer, 0};
    if (pts_ns < epoch_session_pts_ns)
        return SegmentLocalPts{SegmentPlacement::Trim, 0};
    return SegmentLocalPts{SegmentPlacement::Write, pts_ns - epoch_session_pts_ns};
}

} // namespace exosnap::engine
