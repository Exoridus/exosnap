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

namespace recorder_core {

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

// Ceiling on the memory the decoded-frame queue may hold. The queue stores
// BGRA frames, so a depth that is reasonable as a COUNT can be enormous as a
// SIZE: the same 0.2 s window is ~235 MB at 1440p60 but over a gigabyte at
// 2160p120. And because the decode thread blocks only once the queue is full,
// that depth is the steady state during playback, not a transient peak.
//
// 512 MB is chosen to leave every ordinary case (up to 1440p144) on its
// rate-derived depth and bind only on 4K-at-high-rate and on misdeclared
// frame rates.
inline constexpr size_t kDefaultMaxVideoQueueBytes = 512ull * 1024ull * 1024ull;

// Floor the byte budget may never push the queue below: under a handful of
// frames there is no decode-ahead left to ride out a stall, so an oversized
// frame gets a shallow queue rather than none at all.
inline constexpr size_t kMinVideoQueueFrames = 4;

// Frame rates above this are taken as a declaration error rather than a real
// capture rate (Matroska with a millisecond timebase routinely reports
// r_frame_rate = 1000/1). High-speed capture at 240 or 480 fps stays below it.
inline constexpr double kMaxPlausibleFrameRate = 480.0;

// How many decoded frames the queue between the video decode thread and
// PollFrame() must be able to hold.
//
// That queue is the video pipeline's decode-ahead buffer: the decode thread
// fills it and blocks when it is full, so its depth is how much decoded video
// is banked against a hitch before presentation runs dry. Expressed as a
// duration (`decode_ahead_seconds`) rather than a frame count, because the
// useful amount is "enough time to ride out a stall", which is the same
// wall-clock span whatever the clip's frame rate happens to be. The result
// adds a third again as headroom and never falls below a floor, so a clip
// declaring a nonsensical rate still plays.
//
// Deliberately a function of the CLIP's rate rather than a constant: a fixed
// capacity is only ever correct for the one frame rate it was computed for.
//
// bytes_per_frame caps that count against max_queue_bytes, because the rate
// alone says nothing about how much memory the depth costs. Pass 0 when the
// frame size is not known yet (before the first frame is decoded) -- that
// means "no byte information", not "budget for zero frames".
size_t VideoQueueCapacityForFrameRate(double fps, double decode_ahead_seconds, size_t bytes_per_frame,
                                      size_t max_queue_bytes) noexcept;

} // namespace recorder_core
