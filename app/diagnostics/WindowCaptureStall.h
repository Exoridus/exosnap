#pragma once

// Mid-recording honesty for a window capture target that stopped producing
// frames (QCR-804). When a captured window switches into exclusive fullscreen
// *during* a recording, WGC delivers nothing and the CFR encoder silently
// duplicates the last frame — the recording keeps running "green" while the
// video is frozen from that moment on.
//
// This is a CAPTURE-STALL contract, not a fullscreen-detection contract: what is
// proven is the absence of frame progress, never the cause. The cause classifier
// below is used only to stay SILENT where no-frames is legitimate, and to add an
// actionable hint when a fullscreen signal actually corroborates it.
//
// Detection is two-stage so nothing is polled on a healthy recording:
//   Stage 1 (pure, no Win32): WindowCaptureStallMonitor watches
//           CaptureDiagnostics::frames_captured — the count of frames the capture
//           BACKEND produced — across the diagnostics snapshots the pipeline
//           already publishes at ~5 Hz. No new timer, no new probe, no image
//           comparison. Static picture content is not a stall: as long as WGC
//           keeps handing frames over, frames_captured keeps rising.
//
//           Deliberately NOT CaptureDiagnostics::actual_fps, which the aggregator
//           derives from EMITTED frames. During exactly the failure this detects,
//           the CFR pacer keeps emitting duplicates at the target rate, so
//           actual_fps sits at ~60 while the picture is frozen. A gate built on it
//           can never fire — which is why the previous predicate never did.
//
//   Stage 2 (only once starvation is confirmed): gather the window facts once
//           (+ QUNS) and decide whether the silence is legitimate. Positive
//           evidence is required before anything is reported; an ordinary
//           windowed target that simply has nothing to draw stays silent.
//
// Both stages are PURE — the monitor takes its time from the snapshot's own
// elapsed_seconds — so the whole timing and latching contract is unit-pinned
// without a live recording and without a wall-clock sleep.

#include "WindowTargetFacts.h"

#include <cstdint>

#include <recorder_core/pipeline_diagnostics.h>

namespace exosnap::diagnostics {

// A window capture must produce no frame at all for at least this long before a
// stall is even suspected. Long enough that a scheduling hiccup, a compositor
// stutter or a brief GPU hang never trips it; short enough that the user is told
// while there is still a recording to salvage. At the 5 Hz diagnostics cadence
// this is ~50 consecutive snapshots without a single new capture frame.
inline constexpr double kStallStarveSeconds = 10.0;

// One diagnostics snapshot reduced to the four facts Stage 1 reasons over, plus
// the session identity that keeps two recordings apart. Assembled by the caller
// from RecordingDiagnosticsSnapshot; the monitor never sees the engine type.
struct WindowStallSample {
    // RecordingDiagnosticsSnapshot::session_generation. A change means a new
    // recording: the monitor drops everything it knew about the old one.
    uint64_t session_generation = 0;
    // A WGC window target. Display/Region capture never enters this path.
    bool is_window_target = false;
    // The capture is supposed to be producing frames right now (see
    // CaptureProgressExpected). False while paused, initializing, stopping or
    // finished — the starve clock does not run then.
    bool capture_expected = false;
    // CaptureDiagnostics::frames_captured: monotonic count of frames the capture
    // backend actually produced this session.
    uint64_t frames_captured = 0;
    // RecordingDiagnosticsSnapshot::elapsed_seconds — the engine's own session
    // clock, and the monitor's only notion of time.
    double elapsed_seconds = 0.0;
};

// What the monitor asks the caller to do after one sample.
enum class WindowStallSignal : uint8_t {
    None,      // nothing happened
    Starved,   // starvation just crossed the threshold — classify it, then ApplyVerdict()
    Recovered, // frames resumed after a REPORTED stall — clear the standing warning
};

// Stage-2 outcome for a confirmed starvation.
enum class WindowStallVerdict : uint8_t {
    // The window legitimately produces nothing (minimized, cloaked onto another
    // virtual desktop, hidden, or gone). Never reported.
    Legitimate,
    // An ordinary windowed target with nothing to draw. Indistinguishable from a
    // stall with the facts available mid-recording, so ExoSnap stays silent
    // rather than warning about an idle text editor.
    Unknown,
    // A fullscreen-shaped window that has produced no frame for the whole starve
    // window. A fullscreen application that presents nothing for ten seconds is
    // not idle content — this is reported.
    Stalled,
};

// Cause refinement for a confirmed stall. Only used to suppress (Minimized) and
// to earn the actionable second sentence (ExclusiveFullscreen); None means the
// cause is genuinely unknown and must not be claimed.
enum class WindowStallCause : uint8_t {
    None,
    Minimized,
    ExclusiveFullscreen,
};

// PURE. True only for DiagnosticsLifecycle::Recording. Paused, Initializing,
// Stopping, Completed, Failed and Idle all legitimately produce no capture
// frames, and the starve clock must not run through them.
[[nodiscard]] bool CaptureProgressExpected(recorder_core::DiagnosticsLifecycle lifecycle) noexcept;

// PURE. Cause of a confirmed stall. Positive evidence required:
//   minimized                                 -> Minimized
//   fullscreen-shaped + (QUNS || present_fse) -> ExclusiveFullscreen
//   otherwise                                 -> None (unknown: claim nothing)
[[nodiscard]] WindowStallCause EvaluateWindowStall(const WindowTargetFacts& facts, bool present_fse) noexcept;

// PURE. Stage 2: should a confirmed starvation be reported to the user?
// Legitimate silence is filtered out first, then only a fullscreen-shaped window
// clears the bar. `present_fse` is a PresentMon ExclusiveFullscreen observation
// when one is available; it only ever refines the cause, never the verdict.
[[nodiscard]] WindowStallVerdict ClassifyConfirmedStall(const WindowTargetFacts& facts, bool present_fse) noexcept;

// PURE (no Win32, no wall clock, no Qt). Stage 1: watches capture-frame progress
// across diagnostics snapshots and owns the whole latching contract.
//
//   first confirmed stall   -> one Starved signal
//   continued stall         -> nothing (the verdict latches; no per-tick repeat)
//   frames resume           -> one Recovered signal, then healthy again
//   later independent stall -> one new Starved signal
//
// Not thread-safe by design: it lives on, and is only ever driven from, the Qt
// main thread — the diagnostics snapshots reach it through the coordinator's
// queued connection, so no capture-thread state is ever dereferenced here.
class WindowCaptureStallMonitor {
  public:
    // Feed one snapshot. Returns what the caller must act on.
    [[nodiscard]] WindowStallSignal Observe(const WindowStallSample& sample) noexcept;

    // The caller's Stage-2 answer to a Starved signal. MUST be called after every
    // Starved, exactly once: until it arrives the monitor holds the starvation
    // un-latched and will not ask again, and afterwards it will not ask again
    // until frame progress resumes. Stalled latches as reported (so Recovered is
    // owed later); anything else latches as suppressed (silently).
    void ApplyVerdict(WindowStallVerdict verdict) noexcept;

    // Forget everything. Called on a fresh recording start; a session-generation
    // change does the same thing on its own.
    void Reset() noexcept;

    // True while a stall has been reported and frames have not resumed.
    [[nodiscard]] bool reported_stall() const noexcept {
        return phase_ == Phase::Reported;
    }

    // How many independent stalls were REPORTED this session. Feeds the session
    // report so the user can still see it after the recording ended.
    [[nodiscard]] uint32_t reported_episodes() const noexcept {
        return reported_episodes_;
    }

    // Seconds since the last observed capture-frame progress, 0 when unknown.
    // Exposed for logging and tests.
    [[nodiscard]] double seconds_without_progress() const noexcept {
        return seconds_without_progress_;
    }

  private:
    enum class Phase : uint8_t {
        Healthy,         // frames are progressing, or starvation has not matured
        AwaitingVerdict, // Starved was returned; the caller owes ApplyVerdict()
        Reported,        // a stall was reported and is still standing
        Suppressed,      // starvation confirmed but legitimately silent
    };

    Phase phase_ = Phase::Healthy;
    bool have_baseline_ = false;
    uint64_t generation_ = 0;
    uint64_t baseline_frames_ = 0;
    double baseline_elapsed_ = 0.0;
    double seconds_without_progress_ = 0.0;
    uint32_t reported_episodes_ = 0;
};

} // namespace exosnap::diagnostics
