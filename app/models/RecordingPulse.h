#pragma once

// The one heartbeat the shell surfaces share.
//
// Windows has no animated-icon API: Shell_NotifyIcon and
// ITaskbarList3::SetOverlayIcon each take a single HICON, so an animation is a
// timer swapping whole icons.
//
// WHAT MAKES A PERMANENT BEAT AFFORDABLE
// --------------------------------------
// The frames modulate BRIGHTNESS and nothing else. An earlier candidate moved
// the radii as well, and at 16 px two adjacent frames of that differ by well
// under a device pixel: it read as a flicker in the corner of the screen rather
// than as a heartbeat, which is why it ran for two cycles and then stopped.
// Brightness has no sub-pixel problem, so the beat runs for as long as the
// recording does -- which is what a recording indicator is for.
//
// The cost is four icon swaps a second on the UI thread. Capture and encode
// never see it: they are on their own threads, and the shell call is not on any
// path they touch.
//
// One phase, read by every shell surface. A tray timer and a taskbar timer would
// drift apart, and the two icons would then describe the same recording out of
// step.

#include <QtGlobal>

namespace exosnap {

// Six frames over one period. The light travels outwards from the centre dot to
// the inner ring and back, and the first and last frames are the same: the loop
// rests at the bottom for two ticks, which is what makes it a heartbeat rather
// than a metronome. The frames themselves are assets
// (app/assets/brand/marks/recording-f*.svg); only their timing is here.
inline constexpr int kRecordingPulseFrameCount = 6;

// 250 ms a frame, so a second and a half per beat. Slow enough that the
// notification area does not flicker, fast enough that the mark reads as alive,
// and four icon swaps a second is a rate the shell absorbs without a visible
// redraw cost.
inline constexpr int kRecordingPulseIntervalMs = 250;

// Where a recording's beat starts. The bottom of the loop, so every recording
// begins at the same point of the same beat rather than wherever the previous
// one left off.
inline constexpr int kRecordingPulseFirstFrame = 0;

// The processing animation runs at the same cadence. Its frame count differs
// because its loop has no rest in it: a spinner that paused would read as the
// operation having stalled.
inline constexpr int kProcessingFrameIntervalMs = kRecordingPulseIntervalMs;
inline constexpr int kProcessingFrameCount = 4;

// Advances one tick, wrapping. Out-of-range and negative inputs come back to
// frame 0 rather than propagating: this indexes an icon array.
[[nodiscard]] int NextRecordingPulseFrame(int frame, int frame_count = kRecordingPulseFrameCount) noexcept;

} // namespace exosnap
