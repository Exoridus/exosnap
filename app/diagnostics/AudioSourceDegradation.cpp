#include "AudioSourceDegradation.h"

namespace exosnap::diagnostics {

bool AudioDegradationObservable(exosnap::engine::DiagnosticsLifecycle lifecycle) noexcept {
    return lifecycle == exosnap::engine::DiagnosticsLifecycle::Recording ||
           lifecycle == exosnap::engine::DiagnosticsLifecycle::Paused;
}

AudioDegradationSignal AudioSourceDegradationMonitor::Observe(const AudioDegradationSample& sample) noexcept {
    // A callback that belongs to an already-finished session must not touch the
    // current one's latch — neither to raise (the outage is over and was reported
    // then) nor to clear (it would silently drop a standing notice about the
    // recording that is actually running). The coordinator's own generation gate
    // normally drops these; this is the second, local guarantee, and it is why
    // the generation high-water mark survives Reset().
    if (have_generation_ && sample.session_generation < generation_) {
        return AudioDegradationSignal::None;
    }

    // A new recording starts from healthy: a standing notice from the previous
    // session may not be carried into it, and its count may not be mistaken for
    // "unchanged" here.
    if (!have_generation_ || sample.session_generation != generation_) {
        Reset();
        have_generation_ = true;
        generation_ = sample.session_generation;
    }

    // Anything that is not a live recording/paused snapshot reads as "not
    // degraded", so the notice clears on its own when the session ends — exactly
    // what product-spec promises ("clears the moment every source reactivates or
    // the recording ends"). An inconsistent snapshot (source_degraded with a zero
    // count) reads the same way rather than raising a notice about no sources.
    const bool observable = sample.valid && AudioDegradationObservable(sample.lifecycle);
    const uint32_t count = (observable && sample.source_degraded) ? sample.degraded_sources : 0;
    const uint32_t kinds = count > 0 ? sample.degraded_source_kinds : 0;

    if (count == degraded_sources_ && kinds == degraded_source_kinds_) {
        return AudioDegradationSignal::None; // unchanged — never re-announce
    }

    if (count == 0) {
        degraded_sources_ = 0;
        degraded_source_kinds_ = 0;
        return AudioDegradationSignal::Clear;
    }

    // 0 -> n is a new outage; n -> m is the same outage with a different set, and
    // the caller replaces the standing notice in place rather than stacking one.
    if (degraded_sources_ == 0) {
        ++reported_episodes_;
    }
    degraded_sources_ = count;
    degraded_source_kinds_ = kinds;
    return AudioDegradationSignal::Raise;
}

void AudioSourceDegradationMonitor::Reset() noexcept {
    // Latch and per-session bookkeeping only. The session-generation high-water
    // mark is deliberately kept: it is the monitor's memory of which recordings
    // are already over, and dropping it would let a late snapshot from the
    // previous session re-arm the latch it was just reset for.
    degraded_sources_ = 0;
    degraded_source_kinds_ = 0;
    reported_episodes_ = 0;
}

} // namespace exosnap::diagnostics
