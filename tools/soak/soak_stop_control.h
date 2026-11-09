#pragma once

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace exosnap::soak {

// Start and Complete belong to the run owner. RequestStop may be called from
// samplers or console handlers, including before the session is admitted.
// Complete joins the callback before its captured session can be destroyed.
class SoakStopControl {
  public:
    ~SoakStopControl() {
        Complete();
    }

    void Start(std::optional<std::chrono::duration<double>> duration, std::function<void()> stop) {
        if ((duration && (!std::isfinite(duration->count()) || duration->count() <= 0)) || !stop)
            throw std::invalid_argument("A soak deadline requires a positive finite duration and stop callback");
        std::lock_guard lock(mutex_);
        if (started_ || completed_)
            throw std::logic_error("A soak stop controller can only start once");
        started_ = true;
        thread_ = std::thread([this, duration, stop = std::move(stop)] {
            std::unique_lock lock(mutex_);
            const auto done = [this] { return requested_ || completed_; };
            if (duration)
                wake_.wait_for(lock, *duration, done);
            else
                wake_.wait(lock, done);
            const bool should_stop = !completed_;
            lock.unlock();
            if (should_stop)
                stop();
        });
    }

    void RequestStop() {
        {
            std::lock_guard lock(mutex_);
            requested_ = true;
        }
        wake_.notify_all();
    }

    void Complete() {
        {
            std::lock_guard lock(mutex_);
            completed_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }

  private:
    std::mutex mutex_;
    std::condition_variable wake_;
    bool started_ = false;
    bool requested_ = false;
    bool completed_ = false;
    std::thread thread_;
};

// A clean process exit requires both a successful session and readable output.
// This is a demux verdict, not full decode, completeness or hardware acceptance.
inline int SoakExitCode(bool session_ok, bool media_ok, bool evidence_ok, bool aborted) {
    if (!evidence_ok)
        return 2;
    if (aborted)
        return 3;
    if (!session_ok)
        return 1;
    return media_ok ? 0 : 2;
}

} // namespace exosnap::soak
