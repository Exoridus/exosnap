#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace exosnap {

class BackgroundStorageBudget {
  public:
    bool Acquire(uint64_t bytes, const std::atomic<bool>* cancelled = nullptr) {
        if (bytes > kBurstBytes)
            return false;
        std::unique_lock lock(mutex_);
        while (!closed_ && (!cancelled || !cancelled->load())) {
            const auto now = std::chrono::steady_clock::now();
            const double replenished = tokens_ + std::chrono::duration<double>(now - last_refill_).count() * kRateBytes;
            tokens_ = replenished < kBurstBytes ? replenished : kBurstBytes;
            last_refill_ = now;
            if (!paused_ && tokens_ >= static_cast<double>(bytes)) {
                tokens_ -= static_cast<double>(bytes);
                return true;
            }
            wake_.wait_for(lock, std::chrono::milliseconds(20));
        }
        return false;
    }

    void SetPaused(bool paused) {
        {
            std::lock_guard lock(mutex_);
            paused_ = paused;
        }
        wake_.notify_all();
    }

    bool Paused() {
        std::lock_guard lock(mutex_);
        return paused_;
    }

    void Close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        wake_.notify_all();
    }

  private:
    static constexpr uint64_t kBurstBytes = 4ULL * 1024 * 1024;
    static constexpr double kRateBytes = 16.0 * 1024 * 1024;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool closed_ = false;
    bool paused_ = false;
    double tokens_ = kBurstBytes;
    std::chrono::steady_clock::time_point last_refill_ = std::chrono::steady_clock::now();
};

} // namespace exosnap
