#pragma once

// The command channel between a capture hub service's callers and its pump
// thread: an ordered queue that loses nothing, plus the one acknowledgement the
// engine's lease actually needs.
//
// What it replaces, and why: the services used a single `pending_` slot
// (`pending_ = std::move(cmd)`) and acknowledged a lease with a monotonic
// "processed serial >= mine". Subscribe/Unsubscribe are posted from the UI
// thread and RequestEngineLease from the coordinator's prepare/record worker,
// so the two producers genuinely race, and a later post overwrote an
// unprocessed command. The two ways that bit:
//
//   * a Subscribe posted between a LeaseRequest and the pump's next wake-up
//     replaced the lease request outright -- it was never applied to the hub,
//     yet its waiter was released, because the Subscribe's higher serial
//     satisfied "processed >= mine";
//   * the reverse order dropped the Subscribe, and the idle preview silently
//     stayed dark until something else re-subscribed.
//
// The queue fixes the loss. The acknowledgement is now a statement about the
// capture rather than about progress: only a LeaseRequest ever publishes it,
// carrying its own serial, so no later Subscribe can masquerade as the release
// the engine is waiting for. The state half of the contract -- what may open
// while the lease is out -- lives in CaptureHubGate.h.
//
// Threading: Post, WaitForLeaseRelease and Shutdown are callable from any
// thread. WaitAndDrain and PublishLeaseRelease belong to the pump thread.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

#include "services/CaptureHubGate.h"

namespace exosnap {

template <class Payload> class CaptureHubCommandQueue {
  public:
    struct Entry {
        CaptureHubOp op = CaptureHubOp::Unsubscribe;
        uint64_t serial = 0;
        Payload payload{};
    };

    // Returns the command's serial. For a LeaseRequest that serial is what
    // WaitForLeaseRelease() waits on.
    uint64_t Post(CaptureHubOp op, Payload payload = Payload{}) {
        uint64_t serial = 0;
        {
            std::lock_guard lock(mutex_);
            serial = next_serial_++;
            queue_.push_back(Entry{op, serial, std::move(payload)});
        }
        cv_.notify_all();
        return serial;
    }

    // Blocks up to `tick` for work, then hands over every queued command in
    // post order. An empty result is the normal idle case: the pump still runs
    // its cadence.
    void WaitAndDrain(std::chrono::milliseconds tick, std::vector<Entry>& out) {
        out.clear();
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, tick, [this] { return !queue_.empty() || stopping_; });
        out.reserve(queue_.size());
        while (!queue_.empty()) {
            out.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
    }

    void PublishLeaseRelease(uint64_t serial) {
        {
            std::lock_guard lock(mutex_);
            if (serial > lease_released_serial_)
                lease_released_serial_ = serial;
        }
        ack_cv_.notify_all();
    }

    // True once this service has provably released the capture the command with
    // `serial` asked for. False on timeout or shutdown -- the caller logs and
    // proceeds, because a wedged pump thread must not hang the recording start.
    [[nodiscard]] bool WaitForLeaseRelease(uint64_t serial, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        ack_cv_.wait_for(lock, timeout, [this, serial] { return lease_released_serial_ >= serial || stopping_; });
        return lease_released_serial_ >= serial;
    }

    // Wakes the pump thread and every waiter. A lease waiter released this way
    // reports failure rather than blocking for its full timeout against a pump
    // that is never going to answer.
    void Shutdown() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        ack_cv_.notify_all();
    }

    [[nodiscard]] bool Stopping() const {
        std::lock_guard lock(mutex_);
        return stopping_;
    }

    [[nodiscard]] size_t PendingCountForTest() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]] uint64_t LeaseReleasedSerialForTest() const {
        std::lock_guard lock(mutex_);
        return lease_released_serial_;
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable ack_cv_;
    std::deque<Entry> queue_;
    uint64_t next_serial_ = 1;
    uint64_t lease_released_serial_ = 0;
    bool stopping_ = false;
};

} // namespace exosnap
