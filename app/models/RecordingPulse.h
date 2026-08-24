#pragma once

// The one heartbeat the shell surfaces share.
//
// Windows has no animated-icon API: Shell_NotifyIcon and
// ITaskbarList3::SetOverlayIcon each take a single HICON. An animation is
// therefore a timer swapping pre-rendered frames, and the cost of that timer is
// paid for the whole length of a recording -- which is what makes the frame
// count and the interval a product decision rather than an animation budget.
//
// One phase, read by every surface. A tray timer, a taskbar timer and a QML
// timer would drift apart within a minute and the two icons would then be
// describing the same recording out of step.

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
