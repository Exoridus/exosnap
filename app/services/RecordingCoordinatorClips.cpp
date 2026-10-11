#include "RecordingCoordinator.h"
#include "diagnostics/DiskSpaceThresholds.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace exosnap {

void RecordingCoordinator::SetClipJobsChangedCallback(ClipJobsChangedCallback callback) {
    on_clip_jobs_changed_ = std::move(callback);
}

bool RecordingCoordinator::CanRequestRecordingClip() const {
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
    return clip_jobs_->Confirm(job_id, generation);
}

bool RecordingCoordinator::CancelRecordingClip(engine::JobId job_id, uint64_t generation) {
    return clip_jobs_->Cancel(job_id, generation);
}

std::vector<RecordingClipJobSnapshot> RecordingCoordinator::RecordingClipSnapshots() const {
    return clip_jobs_->Snapshots();
}
uint64_t RecordingCoordinator::RecordingStorageReserveBytes() const noexcept {
    const auto extra = primary_clip_reserve_bytes_.load();
    return extra > UINT64_MAX - diagnostics::kHardStopFreeBytes ? UINT64_MAX : diagnostics::kHardStopFreeBytes + extra;
}

void RecordingCoordinator::PostClipJobsChanged() {
    if (!on_clip_jobs_changed_)
        return;
    const std::weak_ptr<int> life = clip_callback_life_.load();
    auto callback = on_clip_jobs_changed_;
    if (QCoreApplication::instance())
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [life, callback = std::move(callback)] {
                if (!life.expired())
                    callback();
            },
            Qt::QueuedConnection);
    else if (!life.expired())
        callback();
}

bool RecordingCoordinator::PauseClipBackground() {
    if (is_remuxing_.load() || pending_segment_remux_jobs_.load() > 0 || active_segment_remux_jobs_.load() > 0)
        return true;
    return background_storage_->Paused();
}

} // namespace exosnap
