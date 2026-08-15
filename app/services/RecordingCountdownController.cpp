#include "RecordingCountdownController.h"

#include <algorithm>

namespace exosnap {

bool RecordingCountdownController::start(int seconds, int64_t now_ms) {
    if (seconds != 3 && seconds != 5 && seconds != 10) {
        return false;
    }
    if (state_ == CountdownState::Running) {
        return false;
    }

    duration_seconds_ = seconds;
    started_at_ms_ = now_ms;
    state_ = CountdownState::Running;
    return true;
}

void RecordingCountdownController::cancel() {
    if (state_ != CountdownState::Running) {
        return;
    }
    state_ = CountdownState::Cancelling;
}

void RecordingCountdownController::reset() {
    state_ = CountdownState::Idle;
    duration_seconds_ = 0;
    started_at_ms_ = 0;
}

void RecordingCountdownController::complete() {
    if (state_ == CountdownState::Running) {
        state_ = CountdownState::Completed;
    }
}

int RecordingCountdownController::remainingSeconds(int64_t now_ms) const noexcept {
    if (state_ != CountdownState::Running || duration_seconds_ <= 0) {
        return 0;
    }

    const int64_t elapsed_ms = (std::max<int64_t>)(0, now_ms - started_at_ms_);
    const int64_t duration_ms = static_cast<int64_t>(duration_seconds_) * 1000;
    const int64_t remaining_ms = (std::max<int64_t>)(0, duration_ms - elapsed_ms);
    if (remaining_ms == 0) {
        return 0;
    }
    return static_cast<int>((remaining_ms + 999) / 1000);
}

double RecordingCountdownController::remainingFraction(int64_t now_ms) const noexcept {
    // The same clock remainingSeconds() reads, without the rounding. The digit
    // wants whole seconds; a ring wants the actual position, and deriving one
    // from the other is a loss that no animation downstream can reverse -- it
    // either steps three times or lags behind by however much it interpolates.
    if (state_ != CountdownState::Running || duration_seconds_ <= 0) {
        return 0.0;
    }
    const int64_t elapsed_ms = (std::max<int64_t>)(0, now_ms - started_at_ms_);
    const int64_t duration_ms = static_cast<int64_t>(duration_seconds_) * 1000;
    const int64_t remaining_ms = (std::max<int64_t>)(0, duration_ms - elapsed_ms);
    return static_cast<double>(remaining_ms) / static_cast<double>(duration_ms);
}

bool RecordingCountdownController::hasReachedZero(int64_t now_ms) const noexcept {
    return state_ == CountdownState::Running && remainingSeconds(now_ms) == 0;
}

} // namespace exosnap
