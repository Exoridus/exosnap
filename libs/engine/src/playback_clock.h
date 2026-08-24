#pragma once

// Pure playback-clock math for the Edit-page video player (no FFmpeg/COM/Qt).
//
// AudioClockMs turns "how many audio frames has the WASAPI render client
// actually written so far" into a playback position in milliseconds -- this
// is the playback master clock (see docs/superpowers/specs/
// 2026-07-14-edit-video-player-design.md).
//
// SelectFrameForClock turns "which decoded video frames are queued, and where
// is the clock now" into "which one to display, and how many older ones to
// drop as real, honest playback drops" -- no catch-up blending, no silent
// resync.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace exosnap::engine {

// frames_played: cumulative audio frames the endpoint has actually PLAYED.
// sample_rate_hz: the render format's sample rate. Returns 0 if
// sample_rate_hz is 0 (defensive -- never divides by zero).
int64_t AudioClockMs(uint64_t frames_played, uint32_t sample_rate_hz) noexcept;

// --- IAudioClock conversion (used by WasapiAudioRenderer) ------------------
//
// The playback clock must count what has been HEARD, not what has been handed
// to the endpoint: a shared-mode render buffer holds up to 200 ms, so a clock
// that advanced on every write ran that far ahead of the sound and made video
// lead audio by the whole buffer depth. IAudioClock::GetPosition is the
// documented play cursor. Its unit is whatever GetFrequency reports (bytes/s
// for a shared-mode stream, frames/s for exclusive mode), so position is only
// meaningful as position/frequency seconds -- these helpers do that conversion
// without assuming either case.

// Frames at target_rate_hz corresponding to `position` device-clock units.
// Returns 0 when frequency or target_rate_hz is 0 (never divides by zero).
uint64_t ClockPositionToFrames(uint64_t position, uint64_t frequency, uint32_t target_rate_hz) noexcept;

// IAudioClock::GetPosition reports the position as of `qpc_position_100ns`,
// which can be up to one device period old. Extrapolate it to `qpc_now_100ns`
// so the clock reads smoothly between device updates instead of stepping.
// The extrapolation is clamped to max_extrapolation_100ns (and never runs
// backwards) so a stale or bogus QPC pair can never push the clock forward
// without bound.
uint64_t InterpolateClockPosition(uint64_t position, uint64_t frequency, uint64_t qpc_position_100ns,
                                  uint64_t qpc_now_100ns, uint64_t max_extrapolation_100ns) noexcept;

// How many audio frames at the FRONT of a decoded block belong to the preroll
// before `start_us` and must not be handed to the renderer.
//
// A playback seek positions on the keyframe at or before the requested start,
// so decoding begins earlier than asked on BOTH streams. The video side
// already discards what it decoded too early; without the same trim on audio,
// the ring receives sound starting at the keyframe while the clock is seeded
// to start_us, and video leads audio by that difference for the entire run --
// up to a full keyframe interval (2 s at the product default).
//
// Returns 0 (keep everything) when the rate is unknown or the block is empty:
// a slightly offset soundtrack beats replacing it with silence. Never returns
// more than block_frame_count.
size_t AudioPrerollFramesToDrop(int64_t block_pts_us, size_t block_frame_count, uint32_t sample_rate_hz,
                                int64_t start_us) noexcept;

struct FrameSelection {
    // Index into the caller's available_pts_ms of the frame to display, or
    // nullopt if the clock is before the first available frame (nothing to
    // show yet -- keep showing whatever is already on screen).
    std::optional<size_t> index;
    // How many frames strictly before the selected index are now stale and
    // should be dropped/discarded by the caller (real drops, not blended).
    size_t dropped_count = 0;
};

// available_pts_ms must be sorted ascending (the natural decode order).
// Picks the LATEST frame whose pts is <= clock_ms. If clock_ms is before the
// first entry, returns {nullopt, 0} (nothing selected, nothing to drop yet).
FrameSelection SelectFrameForClock(std::span<const int64_t> available_pts_ms, int64_t clock_ms) noexcept;

// NOTE (2026-08-03, editor-playback GPU render path): VideoQueueCapacityForFrameRate
// and its kDefaultMaxVideoQueueBytes / kMinVideoQueueFrames / kMaxPlausibleFrameRate
// constants used to live here. They sized EditPlayerSession's bounded video-frame
// queue, which that design removed -- decoded frames now go straight from the decode
// thread into the renderer's single-slot mailbox, paced by the demux thread's
// clock-based read-ahead gate (ShouldDemuxMorePackets, edit_playback_pacing.h) and
// gated at present time by EditPlayerRenderer. With no queue there is no capacity to
// compute, so the function and its constants were deleted rather than kept as
// untested-in-production math.

} // namespace exosnap::engine
