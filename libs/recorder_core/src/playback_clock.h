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

// frames_rendered: cumulative audio frames written to the render endpoint.
// sample_rate_hz: the render format's sample rate. Returns 0 if
// sample_rate_hz is 0 (defensive -- never divides by zero).
int64_t AudioClockMs(uint64_t frames_rendered, uint32_t sample_rate_hz) noexcept;

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

} // namespace recorder_core
