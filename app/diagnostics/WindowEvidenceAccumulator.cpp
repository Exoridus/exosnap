#include "WindowEvidenceAccumulator.h"

namespace exosnap::diagnostics {

void WindowEvidenceAccumulator::Reset(TimePoint now) noexcept {
    started_ = true;
    subscribed_at_ = now;
    last_kind_ = recorder_core::HubFrameKind::None;
    last_generation_ = 0;
    had_fresh_frame_ = false;
    last_fresh_frame_ = now;
    fullscreen_shape_ = false;
    fresh_since_shape_ = false;
}

void WindowEvidenceAccumulator::Update(TimePoint now, recorder_core::HubFrameKind kind, uint64_t generation,
                                       WindowShape shape) noexcept {
    if (!started_) {
        Reset(now);
    }
    last_kind_ = kind;

    // Track the shape transition. Entering FullscreenShaped (re)arms the
    // correlation: no fresh frame counts until one arrives after this point.
    const bool now_fullscreen = shape == WindowShape::FullscreenShaped;
    if (now_fullscreen && !fullscreen_shape_) {
        fresh_since_shape_ = false;
    }
    fullscreen_shape_ = now_fullscreen;

    // A genuinely new frame: generation advanced past the last one we saw.
    if (generation != 0 && generation != last_generation_) {
        last_generation_ = generation;
        had_fresh_frame_ = true;
        last_fresh_frame_ = now;
        if (fullscreen_shape_) {
            fresh_since_shape_ = true;
        }
    }
}

WindowHubEvidence WindowEvidenceAccumulator::Evidence(TimePoint now) const noexcept {
    WindowHubEvidence e;
    e.kind = last_kind_;
    const auto secs = [](TimePoint a, TimePoint b) {
        return std::chrono::duration_cast<std::chrono::duration<double>>(b - a).count();
    };
    e.seconds_subscribed = started_ ? secs(subscribed_at_, now) : 0.0;
    // Until any fresh frame has arrived, "since fresh" is measured from the
    // subscription start, so a source that never produces accumulates time too.
    e.seconds_since_fresh_frame = had_fresh_frame_ ? secs(last_fresh_frame_, now) : e.seconds_subscribed;
    e.fresh_frame_since_fullscreen_shape = fresh_since_shape_;
    return e;
}

} // namespace exosnap::diagnostics
