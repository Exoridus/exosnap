#include "WindowCaptureStall.h"

namespace exosnap::diagnostics {

bool WindowStallSuspected(const WindowStallSnapshot& snapshot) noexcept {
    if (!snapshot.is_window_target) {
        return false;
    }
    // Treat "actual_fps ~0" generously: any sub-frame rate over the starve window
    // means the source is not producing. The rising-duplicate signal confirms the
    // encoder is padding CFR with the held frame rather than the source being
    // legitimately idle-but-alive.
    const bool no_frames = snapshot.actual_fps < 0.5;
    return no_frames && snapshot.frames_duplicated_rising && snapshot.seconds_starved >= kStallStarveSeconds;
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

} // namespace exosnap::diagnostics
