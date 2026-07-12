#pragma once

// Mid-session honesty for a window capture target that stopped producing frames
// (S4). When a game switches into exclusive fullscreen *during* a window
// recording, WGC delivers nothing and the CFR encoder silently duplicates the
// last frame — the recording keeps running "green" while the video is frozen.
//
// Detection is deliberately two-stage so nothing is polled on a healthy
// recording:
//   Stage 1 (cheap, snapshot-only, no Win32): the pipeline snapshot already
//           reports actual_fps and frames_duplicated. WindowStallSuspected() is
//           true only for a window target whose fps has been ~0 for >= 10 s while
//           duplicated frames keep rising. Until that holds, nothing is gathered.
//   Stage 2 (only under suspicion): gather the window facts once (+ QUNS) and
//           classify the cause. Positive evidence is mandatory — a legitimately
//           static window (unknown cause) stays silent.
//
// Both predicates are PURE so the gating and cause logic are unit-pinned without
// a live recording.

#include "WindowTargetFacts.h"

namespace exosnap::diagnostics {

// A window's fps must sit at ~0 for at least this long before a stall is even
// suspected. Long enough that an ordinary static desktop / brief hitch never
// trips it.
inline constexpr double kStallStarveSeconds = 10.0;

// The snapshot fields Stage 1 reasons over. Pulled straight from the live
// pipeline diagnostics on the existing diagnosticsUpdated cadence.
struct WindowStallSnapshot {
    bool is_window_target = false;         // WGC window target (OD monitor never stalls this way)
    double actual_fps = 0.0;               // capture.actual_fps
    bool frames_duplicated_rising = false; // duplicated count grew since the last check
    double seconds_starved = 0.0;          // how long actual_fps has been ~0
};

enum class WindowStallCause {
    None,               // no stall, or a stall whose cause is unknown -> stay silent
    Minimized,          // the window was minimized (frames legitimately stop)
    ExclusiveFullscreen // fullscreen-shaped + a fullscreen signal -> the FSE freeze
};

// PURE. Stage-1 gate: is this window target starved long enough to warrant a
// (cheap) Win32 fact-gather? No facts are read until this is true.
[[nodiscard]] bool WindowStallSuspected(const WindowStallSnapshot& snapshot) noexcept;

// PURE. Stage-2 classification of a suspected stall. Positive evidence required:
//   minimized                               -> Minimized
//   fullscreen-shaped + (QUNS || present_fse) -> ExclusiveFullscreen
//   otherwise                               -> None (unknown: stay silent)
[[nodiscard]] WindowStallCause EvaluateWindowStall(const WindowTargetFacts& facts, bool present_fse) noexcept;

} // namespace exosnap::diagnostics
