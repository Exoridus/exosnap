#include "RecordingCoordinator.h"
#include "diagnostics/DiskSpaceThresholds.h"
#include "models/RecordingCapabilities.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace exosnap {

struct RecordingCoordinator::ClipNotificationState {
    struct Delivery;
    inline static thread_local Delivery* current_delivery = nullptr;

    std::mutex mutex;
    std::condition_variable idle;
    std::shared_ptr<const ClipJobsChangedCallback> callback;
    size_t in_flight = 0;
    bool dirty = false;
    bool queued = false;
    bool closed = false;

    // Callback captures must be released before this guard retires delivery.
    // Shutdown can destroy their borrowed resources as soon as its wait ends.
    struct Delivery {
        explicit Delivery(std::shared_ptr<ClipNotificationState> owner)
            : state(std::move(owner)), previous(current_delivery) {
            current_delivery = this;
        }
        ~Delivery() {
            current_delivery = previous;
            Finish(state);
        }
        std::shared_ptr<ClipNotificationState> state;
        Delivery* previous;
    };

    static void Queue(const std::shared_ptr<ClipNotificationState>& state, QCoreApplication* app) {
        try {
            if (QMetaObject::invokeMethod(app, [state] { Deliver(state); }, Qt::QueuedConnection))
                return;
        } catch (...) {
            std::lock_guard lock(state->mutex);
            state->queued = false;
            throw;
        }
        std::lock_guard lock(state->mutex);
        state->queued = false;
    }

    static void Finish(const std::shared_ptr<ClipNotificationState>& state) {
        {
            std::lock_guard lock(state->mutex);
            --state->in_flight;
        }
        state->idle.notify_all();
    }

    static void Schedule(const std::shared_ptr<ClipNotificationState>& state) {
        QCoreApplication* app = QCoreApplication::instance();
        bool queue = false;
        {
            std::lock_guard lock(state->mutex);
            if (app && !state->closed && state->callback && state->dirty && !state->queued && !state->in_flight) {
                state->queued = true;
                queue = true;
            }
        }
        if (queue)
            Queue(state, app);
    }

    static void Deliver(const std::shared_ptr<ClipNotificationState>& state) {
        std::shared_ptr<const ClipJobsChangedCallback> target_callback;
        {
            std::lock_guard lock(state->mutex);
            state->queued = false;
            if (state->closed || !state->callback || !state->dirty)
                return;
            state->dirty = false;
            ++state->in_flight;
            target_callback = state->callback;
        }
        {
            Delivery delivery(state);
            auto active_callback = std::move(target_callback);
            (*active_callback)();
        }
        Schedule(state);
    }

    static void Notify(const std::shared_ptr<ClipNotificationState>& state) {
        QCoreApplication* app = QCoreApplication::instance();
        std::shared_ptr<const ClipJobsChangedCallback> target_callback;
        {
            std::lock_guard lock(state->mutex);
            if (state->closed || !state->callback)
                return;
            if (app) {
                state->dirty = true;
                if (state->queued || state->in_flight)
                    return;
                state->queued = true;
            } else {
                ++state->in_flight;
                target_callback = state->callback;
            }
        }
        if (app) {
            Queue(state, app);
        } else {
            Delivery delivery(state);
            auto active_callback = std::move(target_callback);
            (*active_callback)();
        }
    }

    void Close() {
        size_t own_deliveries = 0;
        for (auto* delivery = current_delivery; delivery; delivery = delivery->previous)
            if (delivery->state.get() == this)
                ++own_deliveries;
        std::shared_ptr<const ClipJobsChangedCallback> released;
        std::unique_lock lock(mutex);
        closed = true;
        dirty = false;
        released = std::move(callback);
        // A callback can destroy its coordinator. Its stack guard keeps this state
        // alive, while shutdown still joins every delivery on another thread.
        idle.wait(lock, [&] { return in_flight <= own_deliveries; });
        lock.unlock();
    }
};

std::shared_ptr<RecordingCoordinator::ClipNotificationState> RecordingCoordinator::CreateClipNotificationState() {
    return std::make_shared<ClipNotificationState>();
}

void RecordingCoordinator::CloseClipNotifications() {
    clip_notification_state_->Close();
}

void RecordingCoordinator::SetClipJobsChangedCallback(ClipJobsChangedCallback callback) {
    auto replacement = callback ? std::make_shared<const ClipJobsChangedCallback>(std::move(callback)) : nullptr;
    {
        std::lock_guard lock(clip_notification_state_->mutex);
        if (clip_notification_state_->closed)
            return;
        clip_notification_state_->callback.swap(replacement);
        if (!clip_notification_state_->callback)
            clip_notification_state_->dirty = false;
    }
}

bool RecordingCoordinator::CanRequestRecordingClip() const {
    if (!product::kRecordingClipsAvailable)
        return false;
    if (!is_recording_.load() || prepare_in_flight_.load() ||
        !session_.HasCommittedRecordingClipInterval(record_request_.load()))
        return false;
    std::lock_guard lock(markers_mutex_);
    return last_media_time_ns_ > 0 && last_media_time_ns_ <= static_cast<uint64_t>(INT64_MAX);
}

engine::Outcome<engine::JobId> RecordingCoordinator::RequestRecordingClip(int64_t duration_seconds) {
    const auto failure = [](engine::EngineErrorCode code, const char* context) {
        engine::EngineError error;
        error.code = code;
        error.owner = engine::EngineModule::Clip;
        error.context = context;
        return engine::Outcome<engine::JobId>{std::move(error)};
    };
    if (!product::kRecordingClipsAvailable)
        return failure(engine::EngineErrorCode::ProductCapabilityUnavailable,
                       product::kRecordingClipsUnavailableReason);
    if (!CanRequestRecordingClip() || duration_seconds <= 0 || duration_seconds > INT64_MAX / 1000000000)
        return failure(engine::EngineErrorCode::InvalidConfig, "Recording clip duration or media time is unavailable");
    RecordingClipRequest request;
    request.requested_duration_seconds = duration_seconds;
    engine::RecorderConfig config;
    if (!LastCommittedRecorderConfig(&config))
        return failure(engine::EngineErrorCode::InvalidConfig, "Recording clip configuration is unavailable");
    request.cfr = config.cfr;
    {
        std::lock_guard lock(markers_mutex_);
        request.requested_end_r_ns = static_cast<int64_t>(last_media_time_ns_);
        request.markers = markers_;
    }
    request.requested_begin_r_ns = (std::max)(int64_t{0}, request.requested_end_r_ns - duration_seconds * 1000000000);
    auto snapshot = session_.FreezeRecordingMediaIndex(record_request_.load());
    if (!snapshot.succeeded())
        return *snapshot.error();
    request.snapshot = std::move(*snapshot.value());
    {
        std::lock_guard lock(segments_mutex_);
        for (const auto& segment : segments_)
            if (segment.seal && segment.seal->recording_id == request.snapshot.token.recording_id)
                request.closed_segments.push_back(*segment.seal);
    }
    {
        std::lock_guard lock(output_path_mutex_);
        const auto id = QString::fromStdString(engine::ToUuid(engine::NewId<engine::JobId>())).toStdWString();
        request.destination =
            current_output_path_.parent_path() / (current_output_path_.stem().wstring() + L".clip-" + id + L".mkv");
    }
    return clip_jobs_->RequestPlan(std::move(request));
}

bool RecordingCoordinator::ConfirmRecordingClip(engine::JobId job_id, uint64_t generation) {
    return product::kRecordingClipsAvailable && clip_jobs_ && clip_jobs_->Confirm(job_id, generation);
}

bool RecordingCoordinator::CancelRecordingClip(engine::JobId job_id, uint64_t generation) {
    return product::kRecordingClipsAvailable && clip_jobs_ && clip_jobs_->Cancel(job_id, generation);
}

std::vector<RecordingClipJobSnapshot> RecordingCoordinator::RecordingClipSnapshots() const {
    return clip_jobs_ ? clip_jobs_->Snapshots() : std::vector<RecordingClipJobSnapshot>{};
}
uint64_t RecordingCoordinator::RecordingStorageReserveBytes() const noexcept {
    const auto extra = primary_clip_reserve_bytes_.load();
    return extra > UINT64_MAX - diagnostics::kHardStopFreeBytes ? UINT64_MAX : diagnostics::kHardStopFreeBytes + extra;
}

void RecordingCoordinator::PostClipJobsChanged() {
    ClipNotificationState::Notify(clip_notification_state_);
}

bool RecordingCoordinator::PauseClipBackground() {
    if (is_remuxing_.load() || pending_segment_remux_jobs_.load() > 0 || active_segment_remux_jobs_.load() > 0)
        return true;
    return background_storage_->Paused();
}

} // namespace exosnap
