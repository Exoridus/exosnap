#pragma once

// The one heartbeat the shell surfaces share.
//
// Windows has no animated-icon API: Shell_NotifyIcon and
// ITaskbarList3::SetOverlayIcon each take a single HICON, so an animation is a
// timer swapping whole icons.
//
// THE SHELL BEAT IS A TRANSITION, NOT A STATE
// -------------------------------------------
// It runs for a fixed couple of cycles as a recording begins and then holds
// still. A permanent one would keep swapping the notification-area icon and
// redrawing the taskbar button for hours -- and at 16 px the difference between
// two adjacent frames is barely a pixel, so what it buys after the first second
// is a flicker in the corner of the screen rather than a heartbeat. What the
// beat is FOR is the moment the state changed; once the user has seen that, the
// static recording mark is the honest thing to show.
//
// The application's own surfaces are not bound by this. They are a scene graph
// and can animate a dot for free, so the recording indicator inside the window
// breathes for as long as the recording runs. Shell cadence and in-app cadence
// are separate on purpose: the same semantic state, two animation policies.
//
// One phase, read by every shell surface. A tray timer and a taskbar timer would
// drift apart and the two icons would then describe the same recording out of
// step.

#include <QtGlobal>

namespace exosnap {

// Four frames over one period: trough, rise, peak, fall. Three would have no
// symmetric midpoint and would read as a stutter; more frames buy nothing at
// 16 px, where the difference between adjacent steps is already below what the
// eye resolves at this cadence. The frames themselves are assets
// (app/assets/brand/marks/recording-f*.svg) -- what a frame LOOKS like is the
// designer cut's business, and only its timing is here.
inline constexpr int kRecordingPulseFrameCount = 4;

// 4 x 250 ms = one second per beat, near a resting heart rate. Fast enough to
// read as alive, slow enough that the tray does not flicker, and four icon swaps
// a second is a rate the shell absorbs without a visible redraw cost.
inline constexpr int kRecordingPulseIntervalMs = 250;

// The processing animation runs at the same cadence and frame count. One value
// rather than two: the two sequences were authored together and a difference
// between them would be a decision, which is what a second named constant with
// its own reason would then say.
inline constexpr int kProcessingFrameIntervalMs = kRecordingPulseIntervalMs;
inline constexpr int kProcessingFrameCount = kRecordingPulseFrameCount;

// How many full beats a recording entry plays before the shell goes static.
// Two: one alone reads as a glitch, and by the third the eye has stopped
// treating it as information.
inline constexpr int kRecordingPulseTransitionCycles = 2;

// Total ticks in that transition.
inline constexpr int kRecordingPulseTransitionTicks = kRecordingPulseTransitionCycles * kRecordingPulseFrameCount;

// Where the beat ends, and what a static recording therefore shows: the peak,
// which is the frame drawn at full weight. Ending anywhere else would leave the
// tray permanently mid-breath.
inline constexpr int kRecordingPulsePeakFrame = kRecordingPulseFrameCount / 2;

// Advances one tick, wrapping. Out-of-range and negative inputs come back to
// frame 0 rather than propagating: this indexes an icon array.
[[nodiscard]] int NextRecordingPulseFrame(int frame, int frame_count = kRecordingPulseFrameCount) noexcept;

} // namespace exosnap
