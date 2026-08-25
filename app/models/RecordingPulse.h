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
// eye resolves at this cadence.
inline constexpr int kRecordingPulseFrameCount = 4;

// 4 x 220 ms = 880 ms per beat, near a resting heart rate. Fast enough to read
// as alive, slow enough that the tray does not flicker, and 4.5 icon swaps per
// second is a rate the shell absorbs without a visible redraw cost.
inline constexpr int kRecordingPulseIntervalMs = 220;

// How many full beats a recording entry plays before the shell goes static.
// Two: one alone reads as a glitch, and by the third the eye has stopped
// treating it as information.
inline constexpr int kRecordingPulseTransitionCycles = 2;

// Total ticks in that transition.
inline constexpr int kRecordingPulseTransitionTicks = kRecordingPulseTransitionCycles * kRecordingPulseFrameCount;

// Where the beat ends, and what a static recording therefore shows: the peak,
// which is the mark at full weight. Ending anywhere else would leave the tray
// permanently mid-breath.
inline constexpr int kRecordingPulsePeakFrame = kRecordingPulseFrameCount / 2;

// The taskbar overlay badge is a small square in the corner of a taskbar button,
// and Explorer redraws the whole button for every SetOverlayIcon. Two levels is
// what stays legible at that size and halves the shell update rate, while still
// being derived from the same phase as the tray.
inline constexpr int kTaskbarPulseLevels = 2;

// Advances one tick, wrapping. Out-of-range and negative inputs come back to
// frame 0 rather than propagating: this indexes an icon array.
[[nodiscard]] int NextRecordingPulseFrame(int frame, int frame_count = kRecordingPulseFrameCount) noexcept;

// Where the frame sits in the beat: 0.0 at the trough, 1.0 at the peak. A
// triangle rather than a sine -- at four frames the two produce the same three
// numbers, and the triangle says what it is.
[[nodiscard]] double RecordingPulseIntensity(int frame, int frame_count = kRecordingPulseFrameCount) noexcept;

// The intensity quantized into `levels` steps, for a surface that cannot or
// should not render every frame. Same phase, coarser rendering.
[[nodiscard]] int RecordingPulseLevel(int frame, int levels = kTaskbarPulseLevels,
                                      int frame_count = kRecordingPulseFrameCount) noexcept;

} // namespace exosnap
