#pragma once

#include <exosnap/engine/record_request_id.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace exosnap::engine {

[[nodiscard]] inline constexpr bool RecordRequestMatches(RecordRequestId active_request,
                                                         RecordRequestId request_id) noexcept {
    return active_request == kUnscopedRecordRequest || request_id == kUnscopedRecordRequest ||
           active_request == request_id;
}

// Caller serializes this with the session-state mutex. A stop received before
// admission survives only into the recording that carries its request id.
class PendingStopTracker {
  public:
    void NoteStop(bool recording, RecordRequestId request_id) {
        if (!recording) {
            pending_ = true;
            pending_request_ = request_id;
        }
    }

    [[nodiscard]] bool Consume(RecordRequestId request_id) {
        const bool pending = std::exchange(pending_, false);
        const RecordRequestId noted = std::exchange(pending_request_, kUnscopedRecordRequest);
        return pending && RecordRequestMatches(request_id, noted);
    }

    void Clear() {
        pending_ = false;
        pending_request_ = kUnscopedRecordRequest;
    }

  private:
    bool pending_ = false;
    RecordRequestId pending_request_ = kUnscopedRecordRequest;
};

// The request id and pending stop are guarded by the same mutex as the selected
// shared state. Recording denotes running workers, not the preparation phase.
struct SessionRequestGate {
    RecordRequestId active_request = kUnscopedRecordRequest;
    std::atomic<bool> recording{false};
    PendingStopTracker pending_stop;

    template <typename Mutex, typename State>
    std::shared_ptr<State> SelectForStop(Mutex& mutex, const std::shared_ptr<State>& state,
                                         RecordRequestId request_id) {
        std::lock_guard lock(mutex);
        const bool running = recording.load();
        if (!RecordRequestMatches(active_request, request_id))
            return nullptr;
        pending_stop.NoteStop(running, request_id);
        return state;
    }

    template <typename Mutex, typename State>
    std::shared_ptr<State> SelectForSplit(Mutex& mutex, const std::shared_ptr<State>& state,
                                          RecordRequestId request_id) {
        std::lock_guard lock(mutex);
        if (!recording.load() || !RecordRequestMatches(active_request, request_id))
            return nullptr;
        return state;
    }
};

// Retire the admitted request on every return, including preparation failures.
// The next pre-admission stop must not be compared against a finished request.
class SessionRequestScope {
  public:
    SessionRequestScope(std::mutex& mutex, SessionRequestGate& requests) : mutex_(mutex), requests_(requests) {
    }
    SessionRequestScope(const SessionRequestScope&) = delete;
    SessionRequestScope& operator=(const SessionRequestScope&) = delete;
    ~SessionRequestScope() {
        std::lock_guard lock(mutex_);
        requests_.recording.store(false);
        requests_.pending_stop.Clear();
        requests_.active_request = kUnscopedRecordRequest;
    }

  private:
    std::mutex& mutex_;
    SessionRequestGate& requests_;
};

} // namespace exosnap::engine
