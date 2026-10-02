#pragma once

#include <chrono>

namespace exosnap {

constexpr int NormalizePreviewRate(int rate) noexcept {
    return rate == 0 || rate == 15 || rate == 30 || rate == 60 || rate == 120 ? rate : 60;
}

class PreviewRateGate {
  public:
    using Clock = std::chrono::steady_clock;
    bool Take(Clock::time_point now, int rate) noexcept {
        if (rate <= 0 || now < next_)
            return false;
        next_ = now + std::chrono::nanoseconds(1'000'000'000 / rate);
        return true;
    }
    void Reset() noexcept {
        next_ = {};
    }

  private:
    Clock::time_point next_{};
};

} // namespace exosnap
