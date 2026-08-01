#pragma once

// Pure pacing predicate for the Edit-page player's video decode thread
// (docs/superpowers/specs/2026-08-01-edit-player-decoupled-decode-design.md).
//
// The colour conversion is the expensive half of that thread's work (2.9 ms
// per 1440p frame even vectorised), so a frame whose presentation time the
// playback clock has already passed is discarded BEFORE it is converted --
// PollFrame() would only drop it again, and the conversion would have bought
// nothing. This makes the expensive work scale with the presentation rate
// rather than with the clip's frame rate.
//
// Kept here, separate from the FFmpeg-facing engine, so it is unit-testable
// without a real file or a real audio device.

#include <cstdint>

namespace recorder_core {

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

} // namespace recorder_core
