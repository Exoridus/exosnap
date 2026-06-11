#pragma once

#include <cstdint>

namespace exosnap {

enum class CountdownState {
    Idle,
    Running,
    Cancelling,
    Completed,
};

class RecordingCountdownController {
  public:
    bool start(int seconds, int64_t now_ms);
    void cancel();
    void reset();
    void complete();

    [[nodiscard]] CountdownState state() const noexcept {
        return state_;
    }
    [[nodiscard]] bool isRunning() const noexcept {
        return state_ == CountdownState::Running;
    }
    [[nodiscard]] int durationSeconds() const noexcept {
        return duration_seconds_;
    }
    [[nodiscard]] int remainingSeconds(int64_t now_ms) const noexcept;
    [[nodiscard]] bool hasReachedZero(int64_t now_ms) const noexcept;

  private:
    CountdownState state_ = CountdownState::Idle;
    int duration_seconds_ = 0;
    int64_t started_at_ms_ = 0;
};

} // namespace exosnap
