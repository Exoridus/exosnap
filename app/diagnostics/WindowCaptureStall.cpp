#include "WindowCaptureStall.h"

namespace exosnap::diagnostics {

bool CaptureProgressExpected(exosnap::engine::DiagnosticsLifecycle lifecycle) noexcept {
    return lifecycle == exosnap::engine::DiagnosticsLifecycle::Recording;
}

WindowStallCause EvaluateWindowStall(const WindowTargetFacts& facts, bool present_fse) noexcept {
    if (facts.minimized) {
        return WindowStallCause::Minimized;
    }
    if (ClassifyWindowShape(facts) == WindowShape::FullscreenShaped && (facts.quns_d3d_fullscreen || present_fse)) {
        return WindowStallCause::ExclusiveFullscreen;
    }
    return WindowStallCause::None;
}

WindowStallVerdict ClassifyConfirmedStall(const WindowTargetFacts& facts, bool present_fse) noexcept {
    // A window that is gone, hidden, minimized or cloaked onto another virtual
    // desktop is SUPPOSED to stop producing frames. Reporting that as a stall
    // would be a false alarm about a state the user created deliberately.
    if (!facts.valid || !facts.visible || facts.cloaked) {
        return WindowStallVerdict::Legitimate;
    }
    if (EvaluateWindowStall(facts, present_fse) == WindowStallCause::Minimized) {
        return WindowStallVerdict::Legitimate;
    }
    // An ordinary windowed target with a caption, or one that does not cover its
    // monitor, may simply have nothing to redraw. Mid-recording there is no fact
    // that separates that from a stall, so ExoSnap says nothing rather than
    // warning about an idle text editor.
    if (ClassifyWindowShape(facts) != WindowShape::FullscreenShaped) {
        return WindowStallVerdict::Unknown;
    }
    return WindowStallVerdict::Stalled;
}

WindowStallSignal WindowCaptureStallMonitor::Observe(const WindowStallSample& sample) noexcept {
    // A new recording invalidates everything: neither the starve clock nor a
    // standing stall from the previous session may reach this one. Same rule for
    // a target swap, which the engine stamps as a new generation. The first
    // sample of a session only establishes the baseline.
    if (!have_baseline_ || sample.session_generation != generation_) {
        Reset();
        generation_ = sample.session_generation;
        have_baseline_ = true;
        baseline_frames_ = sample.frames_captured;
        baseline_elapsed_ = sample.elapsed_seconds;
        return WindowStallSignal::None;
    }

    // Display and region capture never enter this path (QCR-804 is a WGC
    // window-capture failure mode), and neither does anything that is not
    // currently expected to produce frames — paused, preparing, stopping, done.
    // The baseline moves with them so a pause neither accumulates starve time nor
    // clears a stall that is still standing.
    if (!sample.is_window_target || !sample.capture_expected) {
        baseline_frames_ = sample.frames_captured;
        baseline_elapsed_ = sample.elapsed_seconds;
        seconds_without_progress_ = 0.0;
        return WindowStallSignal::None;
    }

    // Frame progress. Also covers a counter that went backwards or a clock that
    // did (neither should happen; both re-baseline rather than fabricating a
    // negative starve time).
    if (sample.frames_captured != baseline_frames_ || sample.elapsed_seconds < baseline_elapsed_) {
        const Phase previous = phase_;
        baseline_frames_ = sample.frames_captured;
        baseline_elapsed_ = sample.elapsed_seconds;
        seconds_without_progress_ = 0.0;
        phase_ = Phase::Healthy;
        return previous == Phase::Reported ? WindowStallSignal::Recovered : WindowStallSignal::None;
    }

    seconds_without_progress_ = sample.elapsed_seconds - baseline_elapsed_;
    if (phase_ != Phase::Healthy || seconds_without_progress_ < kStallStarveSeconds) {
        // Already asked (AwaitingVerdict), already latched (Reported/Suppressed),
        // or not starved long enough. Either way: no repeated signal.
        return WindowStallSignal::None;
    }
    phase_ = Phase::AwaitingVerdict;
    return WindowStallSignal::Starved;
}

void WindowCaptureStallMonitor::ApplyVerdict(WindowStallVerdict verdict) noexcept {
    if (phase_ != Phase::AwaitingVerdict) {
        return; // no outstanding question
    }
    if (verdict == WindowStallVerdict::Stalled) {
        phase_ = Phase::Reported;
        ++reported_episodes_;
        return;
    }
    phase_ = Phase::Suppressed;
}

void WindowCaptureStallMonitor::Reset() noexcept {
    phase_ = Phase::Healthy;
    have_baseline_ = false;
    generation_ = 0;
    baseline_frames_ = 0;
    baseline_elapsed_ = 0.0;
    seconds_without_progress_ = 0.0;
    reported_episodes_ = 0;
}

} // namespace exosnap::diagnostics
