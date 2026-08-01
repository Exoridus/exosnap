#pragma once

// Pure pacing predicates for the Edit-page player's three-thread playback
// topology (docs/superpowers/specs/2026-08-01-edit-player-decoupled-decode-design.md).
//
// Kept here, separate from the FFmpeg-facing engine, so they are unit-testable
// without a real file or a real audio device.

#include <cstddef>
#include <cstdint>

namespace recorder_core {

// ---- Video thread: convert-before-present gate ---------------------------
//
// The colour conversion is the expensive half of that thread's work (2.9 ms
// per 1440p frame even vectorised), so a frame whose presentation time the
// playback clock has already passed is discarded BEFORE it is converted --
// PollFrame() would only drop it again, and the conversion would have bought
// nothing. This makes the expensive work scale with the presentation rate
// rather than with the clip's frame rate.

// True when a decoded frame at pts_us is still worth converting.
//
// current_media_time_us is the playback clock in absolute media time. A
// NEGATIVE value means "no clock available" (nothing is playing it back, e.g.
// a throughput probe or a session with no audio stream) -- nothing is then
// known about what is late, so nothing is discarded.
[[nodiscard]] constexpr bool ShouldConvertDecodedFrame(int64_t pts_us, int64_t current_media_time_us) noexcept {
    if (current_media_time_us < 0)
        return true;
    return pts_us >= current_media_time_us;
}

// ---- Demux thread: how far ahead of the playback position to read --------
//
// The demuxer's read-ahead is bounded by the PLAYBACK POSITION, not by how
// full the slowest consumer's queue happens to be. That distinction is the
// whole point: a queue-occupancy bound couples the demuxer's forward progress
// 1:1 to whichever consumer drains slowest, so that consumer's cadence (a
// video thread that spends 120 ms inside its present callback, say) is
// imprinted on the OTHER stream's packet delivery. Bounding by the clock
// instead keeps the demuxer advancing while a consumer is busy, so audio
// packets keep arriving at the rate the clock advances rather than in bursts
// tied to video.

// Sentinel for "the demuxer has not read a usable timestamp yet".
inline constexpr int64_t kUnknownDemuxPositionUs = INT64_MIN;

// The demuxer's read position across both streams: media is only buffered
// through the point BOTH streams have reached. Taking the leading stream
// instead would stop the read at a container-interleave boundary where the
// trailing stream's packets are still unread, which shows up as a gap in that
// stream's delivery. Unknown as long as either stream has no usable timestamp.
[[nodiscard]] constexpr int64_t DemuxedThroughUs(int64_t a_us, int64_t b_us) noexcept {
    if (a_us == kUnknownDemuxPositionUs || b_us == kUnknownDemuxPositionUs)
        return kUnknownDemuxPositionUs;
    return (a_us < b_us) ? a_us : b_us;
}

// True when the demuxer may read one more packet right now.
//
// demuxed_through_us is the largest presentation timestamp handed to a decode
// thread so far. current_media_time_us follows the same convention as above:
// NEGATIVE means no clock exists, and then nothing paces the demuxer (maximum
// throughput -- what the throughput probe and video-only sessions want).
[[nodiscard]] constexpr bool ShouldDemuxMorePackets(int64_t demuxed_through_us, int64_t current_media_time_us,
                                                    int64_t read_ahead_us) noexcept {
    if (current_media_time_us < 0)
        return true;
    if (demuxed_through_us == kUnknownDemuxPositionUs)
        return true; // no usable timestamps yet: nothing to pace against
    return demuxed_through_us < current_media_time_us + read_ahead_us;
}

// ---- Demux thread: per-queue admission -----------------------------------
//
// Each stream's packet queue has a SOFT capacity (the normal buffer target)
// and a HARD one (a pure memory backstop, generous enough that it never binds
// in normal playback). Waiting at the soft limit is what bounds memory, but
// waiting there while the OTHER stream is running dry is exactly the coupling
// this topology exists to remove -- a single demuxer reads in container
// interleave order, so a wait in front of a video packet also withholds the
// audio packets sitting right behind it.
//
// So: at or above the soft limit the demuxer keeps inserting as long as the
// other stream is below its low-water mark, and only truly waits at the hard
// limit. The rule is symmetric -- neither stream is privileged.

struct PacketQueueLimits {
    int64_t soft_capacity_us = 0;     // normal buffer target per stream
    int64_t hard_capacity_us = 0;     // memory backstop (duration)
    size_t hard_capacity_packets = 0; // memory backstop (count), for containers with no packet durations
    int64_t peer_low_water_us = 0;    // below this, the other stream counts as starving
};

struct PacketAdmissionState {
    bool aborted = false;        // shutdown in progress -- beats every other rule
    int64_t queued_us = 0;       // this stream's buffered duration
    size_t queued_packets = 0;   // this stream's packet count
    bool peer_consuming = false; // is there another stream with a live consumer?
    int64_t peer_queued_us = 0;  // that stream's buffered duration
};

// True when the demuxer may insert one more packet into this stream's queue
// without waiting.
[[nodiscard]] constexpr bool ShouldAdmitDemuxedPacket(const PacketAdmissionState& state,
                                                      const PacketQueueLimits& limits) noexcept {
    if (state.aborted)
        return false;
    if (state.queued_us >= limits.hard_capacity_us || state.queued_packets >= limits.hard_capacity_packets)
        return false; // hard limit: always wait, whatever the other stream is doing
    if (state.queued_us < limits.soft_capacity_us)
        return true;
    // At or above the soft limit: only keep going while the other stream is
    // starving and would be withheld by a wait here.
    return state.peer_consuming && state.peer_queued_us < limits.peer_low_water_us;
}

} // namespace recorder_core
