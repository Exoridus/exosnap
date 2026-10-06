#pragma once

#include <exosnap/engine/packet_types.h>
#include <exosnap/engine/split_trigger_source.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>

namespace exosnap::engine {

struct VideoEosSentinel {};
struct VideoProgressSentinel {
    uint64_t safe_before_pts_ns = 0;
};
struct AudioEosSentinel {
    uint32_t track_id = 0;
};
struct SplitSentinel {
    uint32_t new_segment_index = 0;
    SplitTriggerSource trigger = SplitTriggerSource::ManualButton;
};
struct MuxItem {
    std::variant<EncodedVideoPacket, EncodedAudioPacket, VideoEosSentinel, AudioEosSentinel, SplitSentinel,
                 VideoProgressSentinel>
        payload;
};

enum class MuxQueueWait { Ready, Stopping, Failed, TimedOut };

class MuxQueue {
  public:
    struct Limits {
        size_t packets = 2048;
        size_t bytes = 256ull * 1024 * 1024;
        std::chrono::milliseconds timeout{10000};
    };

    void Configure(Limits limits) {
        std::lock_guard lock(mutex_);
        limits_ = limits;
    }

    // The bound is checked before a packet, as a large packet may itself cross
    // the byte limit. A split and its first keyframe are inserted under one lock.
    // Stop permits the bounded final drain; failure interrupts a full-queue wait.
    template <typename Failed, typename Stopping, typename OnWait>
    [[nodiscard]] MuxQueueWait Push(MuxItem item, Failed failed, Stopping stopping, OnWait on_wait,
                                    std::optional<SplitSentinel> split = std::nullopt) {
        std::unique_lock lock(mutex_);
        if (Full()) {
            const auto started = std::chrono::steady_clock::now();
            const auto deadline = started + limits_.timeout;
            const auto finish = [&](MuxQueueWait result) {
                const auto now = std::chrono::steady_clock::now();
                on_wait(now, std::chrono::duration<double, std::milli>(now - started).count());
                return result;
            };
            while (Full()) {
                if (failed()) {
                    return finish(MuxQueueWait::Failed);
                }
                ++waiting_producers_;
                waiting_cv_.notify_all();
                const auto status = space_cv_.wait_until(lock, deadline);
                --waiting_producers_;
                if (status == std::cv_status::timeout && Full()) {
                    if (failed()) {
                        return finish(MuxQueueWait::Failed);
                    }
                    return finish(stopping() ? MuxQueueWait::Stopping : MuxQueueWait::TimedOut);
                }
            }
            finish(MuxQueueWait::Ready);
        }
        if (split) {
            PushLocked(MuxItem{*split});
        }
        PushLocked(std::move(item));
        return MuxQueueWait::Ready;
    }

    // Only typed control records bypass capacity. Payload packets cannot use
    // this path, including packets whose encoded payload happens to be empty.
    void PushSentinel(VideoEosSentinel sentinel) {
        std::lock_guard lock(mutex_);
        PushLocked(MuxItem{sentinel});
    }
    void PushSentinel(AudioEosSentinel sentinel) {
        std::lock_guard lock(mutex_);
        PushLocked(MuxItem{sentinel});
    }
    void PushSentinel(VideoProgressSentinel sentinel) {
        std::lock_guard lock(mutex_);
        PushLocked(MuxItem{sentinel});
    }

    [[nodiscard]] std::optional<MuxItem> Pop() {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        MuxItem item = std::move(queue_.front());
        queue_.pop_front();
        bytes_ -= PayloadBytes(item);
        space_cv_.notify_all();
        return item;
    }

    template <typename Stopping> size_t WaitForItems(Stopping stopping) {
        std::unique_lock lock(mutex_);
        items_cv_.wait_for(lock, std::chrono::milliseconds(5), [&] { return !queue_.empty() || stopping(); });
        return queue_.size();
    }

    void NotifyStop() noexcept {
        // Pair the wake with the waiter's mutex so a stop/failure cannot land
        // between its predicate check and the release-and-wait operation.
        std::lock_guard lock(mutex_);
        items_cv_.notify_all();
        space_cv_.notify_all();
    }

    void Reset() {
        std::lock_guard lock(mutex_);
        queue_.clear();
        bytes_ = 0;
        space_cv_.notify_all();
    }

    [[nodiscard]] size_t Size() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]] size_t Bytes() const {
        std::lock_guard lock(mutex_);
        return bytes_;
    }

    [[nodiscard]] std::deque<MuxItem> Snapshot() const {
        std::lock_guard lock(mutex_);
        return queue_;
    }

    // A synchronized observation of a producer entering the actual CV wait,
    // used by deterministic concurrency tests without scheduler sleeps.
    [[nodiscard]] bool WaitForBlockedProducer(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return waiting_cv_.wait_for(lock, timeout, [&] { return waiting_producers_ != 0; });
    }

  private:
    [[nodiscard]] bool Full() const {
        return queue_.size() >= limits_.packets || bytes_ >= limits_.bytes;
    }

    [[nodiscard]] static size_t PayloadBytes(const MuxItem& item) {
        if (const auto* video = std::get_if<EncodedVideoPacket>(&item.payload)) {
            return video->bytes.size();
        }
        if (const auto* audio = std::get_if<EncodedAudioPacket>(&item.payload)) {
            return audio->bytes.size();
        }
        return 0;
    }

    void PushLocked(MuxItem item) {
        const size_t bytes = PayloadBytes(item);
        queue_.push_back(std::move(item));
        bytes_ += bytes;
        items_cv_.notify_one();
    }

    mutable std::mutex mutex_;
    std::condition_variable items_cv_;
    std::condition_variable space_cv_;
    std::condition_variable waiting_cv_;
    std::deque<MuxItem> queue_;
    size_t bytes_ = 0;
    size_t waiting_producers_ = 0;
    Limits limits_;
};

} // namespace exosnap::engine
