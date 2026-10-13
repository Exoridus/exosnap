#include "RecordingClipAdapter.h"
#include "models/RecordingCapabilities.h"

#include <QStringList>
#include <QVariantMap>

#include <algorithm>
#include <string>
#include <utility>

namespace exosnap::quick {
namespace {

QString TimeText(int64_t time_ns) {
    const auto seconds = std::max<int64_t>(0, time_ns) / 1'000'000'000;
    return QStringLiteral("%1:%2:%3")
        .arg(seconds / 3600, 2, 10, QChar(u'0'))
        .arg((seconds / 60) % 60, 2, 10, QChar(u'0'))
        .arg(seconds % 60, 2, 10, QChar(u'0'));
}

QString PhaseText(RecordingClipPhase phase) {
    switch (phase) {
    case RecordingClipPhase::Planning:
        return RecordingClipAdapter::tr("Planning clip");
    case RecordingClipPhase::Review:
        return RecordingClipAdapter::tr("Review clip range");
    case RecordingClipPhase::Queued:
        return RecordingClipAdapter::tr("Clip queued");
    case RecordingClipPhase::Reading:
        return RecordingClipAdapter::tr("Reading recording");
    case RecordingClipPhase::Remuxing:
        return RecordingClipAdapter::tr("Saving clip");
    case RecordingClipPhase::Finalizing:
        return RecordingClipAdapter::tr("Finalizing clip");
    case RecordingClipPhase::VerifyingMetadata:
        return RecordingClipAdapter::tr("Verifying metadata");
    case RecordingClipPhase::Publishing:
        return RecordingClipAdapter::tr("Publishing clip");
    case RecordingClipPhase::Completed:
        return RecordingClipAdapter::tr("Clip saved");
    case RecordingClipPhase::Cancelling:
        return RecordingClipAdapter::tr("Cancelling clip");
    case RecordingClipPhase::Cancelled:
        return RecordingClipAdapter::tr("Clip cancelled");
    case RecordingClipPhase::Failed:
        return RecordingClipAdapter::tr("Clip failed");
    }
    return {};
}

QString ErrorText(const engine::EngineError& error) {
    switch (error.code) {
    case engine::EngineErrorCode::ProductCapabilityUnavailable:
        return RecordingClipAdapter::tr("Recording Clips are unavailable in this product version");
    case engine::EngineErrorCode::StorageInsufficient:
        return RecordingClipAdapter::tr("Not enough space to save the clip.");
    case engine::EngineErrorCode::ResourceBudgetExceeded:
        if (error.retryability == engine::Retryability::Transient)
            return RecordingClipAdapter::tr("The clip queue is full. Try again after another clip finishes.");
        return RecordingClipAdapter::tr("The clip could not be saved.");
    case engine::EngineErrorCode::IndexNotReady:
        return RecordingClipAdapter::tr("Recording history is not ready for a clip yet.");
    case engine::EngineErrorCode::UnsupportedFormat:
        return RecordingClipAdapter::tr("This clip format is not supported.");
    case engine::EngineErrorCode::SourceIntegrityFailure:
        return RecordingClipAdapter::tr("The recording changed while saving the clip.");
    default:
        return RecordingClipAdapter::tr("The clip could not be saved.");
    }
}

bool CanCancel(RecordingClipPhase phase) {
    return phase != RecordingClipPhase::Completed && phase != RecordingClipPhase::Cancelled &&
           phase != RecordingClipPhase::Failed && phase != RecordingClipPhase::Cancelling;
}

QVariantMap Row(const RecordingClipJobSnapshot& snapshot) {
    QStringList tracks;
    for (const auto& name : snapshot.track_names)
        tracks.push_back(QString::fromStdString(name));
    const bool has_actual = snapshot.actual_begin_r_ns && snapshot.actual_end_r_ns;
    const bool completed = snapshot.phase == RecordingClipPhase::Completed && snapshot.asset_id && snapshot.completed &&
                           snapshot.completed->succeeded;
    QVariantMap row{
        {QStringLiteral("jobId"), QString::fromStdString(engine::ToUuid(snapshot.job_id))},
        {QStringLiteral("generation"), QString::number(snapshot.generation)},
        {QStringLiteral("phase"), static_cast<int>(snapshot.phase)},
        {QStringLiteral("phaseText"), PhaseText(snapshot.phase)},
        {QStringLiteral("requestedBegin"), TimeText(snapshot.requested_begin_r_ns)},
        {QStringLiteral("requestedEnd"), TimeText(snapshot.requested_end_r_ns)},
        {QStringLiteral("requestedDurationSeconds"), QString::number(snapshot.requested_duration_seconds)},
        {QStringLiteral("actualBegin"), has_actual ? TimeText(*snapshot.actual_begin_r_ns) : QString()},
        {QStringLiteral("actualEnd"), has_actual ? TimeText(*snapshot.actual_end_r_ns) : QString()},
        {QStringLiteral("hasActual"), has_actual},
        {QStringLiteral("trackNames"), tracks},
        {QStringLiteral("reviewReason"), RecordingClipAdapter::tr(snapshot.review_reason.c_str())},
        {QStringLiteral("sourceBytesRead"), QString::number(snapshot.source_bytes_read)},
        {QStringLiteral("stagingBytesWritten"), QString::number(snapshot.staging_bytes_written)},
        {QStringLiteral("reservedBytes"), QString::number(snapshot.reserved_bytes)},
        {QStringLiteral("canConfirm"),
         RecordingClipAdapter::productAvailable() && snapshot.phase == RecordingClipPhase::Review && has_actual},
        {QStringLiteral("canCancel"), RecordingClipAdapter::productAvailable() && CanCancel(snapshot.phase)},
        {QStringLiteral("canOpen"), completed},
        {QStringLiteral("assetId"), completed ? QString::fromStdString(engine::ToUuid(*snapshot.asset_id)) : QString()},
        {QStringLiteral("errorSummary"), snapshot.error ? ErrorText(*snapshot.error) : QString()},
        {QStringLiteral("errorDetail"), snapshot.error ? QString::fromStdString(snapshot.error->context) : QString()}};
    return row;
}

} // namespace

RecordingClipAdapter::RecordingClipAdapter(QObject* parent) : QObject(parent) {
    setAvailability(false, {});
}

bool RecordingClipAdapter::productAvailable() noexcept {
    return product::kRecordingClipsAvailable;
}

bool RecordingClipAdapter::canRequest() const noexcept {
    return can_request_;
}

const QString& RecordingClipAdapter::unavailableReason() const noexcept {
    return unavailable_reason_;
}

const QVariantList& RecordingClipAdapter::jobs() const noexcept {
    return jobs_;
}

QString RecordingClipAdapter::preferredDurationSeconds() const {
    return QString::number(preferred_duration_seconds_);
}

QVariantList RecordingClipAdapter::durationOptions() {
    return {QVariantMap{{QStringLiteral("seconds"), QStringLiteral("30")},
                        {QStringLiteral("label"), tr("Last 30 seconds")}},
            QVariantMap{{QStringLiteral("seconds"), QStringLiteral("300")},
                        {QStringLiteral("label"), tr("Last 5 minutes")}},
            QVariantMap{{QStringLiteral("seconds"), QStringLiteral("1800")},
                        {QStringLiteral("label"), tr("Last 30 minutes")}}};
}

void RecordingClipAdapter::setAvailability(bool available, QString reason) {
    available = productAvailable() && available;
    if (!productAvailable())
        reason = tr("Recording Clips are unavailable in this product version");
    if (can_request_ == available && unavailable_reason_ == reason)
        return;
    can_request_ = available;
    unavailable_reason_ = std::move(reason);
    emit availabilityChanged();
}

void RecordingClipAdapter::setJobs(std::vector<RecordingClipJobSnapshot> snapshots) {
    for (auto& incoming : snapshots) {
        const auto previous = std::find_if(snapshots_.begin(), snapshots_.end(),
                                           [&](const auto& old) { return old.job_id == incoming.job_id; });
        if (previous != snapshots_.end() &&
            (previous->generation > incoming.generation ||
             (previous->generation == incoming.generation && previous->phase == RecordingClipPhase::Completed &&
              incoming.phase != RecordingClipPhase::Completed)))
            incoming = *previous;
    }
    QVariantList rows;
    for (const auto& snapshot : snapshots)
        rows.push_back(Row(snapshot));
    snapshots_ = std::move(snapshots);
    if (rows == jobs_)
        return;
    jobs_ = std::move(rows);
    emit jobsChanged();
}

void RecordingClipAdapter::requestPlan(const QString& duration_seconds) {
    bool parsed = false;
    const auto seconds = duration_seconds.toLongLong(&parsed);
    if (can_request_ && parsed && seconds > 0)
        emit planRequested(seconds);
}

void RecordingClipAdapter::acceptConfirmedDuration(int64_t seconds) {
    if (seconds <= 0 || preferred_duration_seconds_ == seconds)
        return;
    preferred_duration_seconds_ = seconds;
    emit preferredDurationChanged();
}

void RecordingClipAdapter::confirmPlan(const QString& job_id, const QString& generation) {
    if (!productAvailable())
        return;
    const auto id = engine::FromUuid<engine::JobId>(job_id.toStdString());
    bool parsed = false;
    const auto value = generation.toULongLong(&parsed);
    if (!id || !parsed)
        return;
    const auto match = std::find_if(snapshots_.begin(), snapshots_.end(), [&](const auto& snapshot) {
        return snapshot.job_id == *id && snapshot.generation == value && snapshot.phase == RecordingClipPhase::Review &&
               snapshot.actual_begin_r_ns && snapshot.actual_end_r_ns;
    });
    if (match != snapshots_.end()) {
        emit confirmRequested(*id, value);
    }
}

void RecordingClipAdapter::cancelJob(const QString& job_id, const QString& generation) {
    if (!productAvailable())
        return;
    const auto id = engine::FromUuid<engine::JobId>(job_id.toStdString());
    bool parsed = false;
    const auto value = generation.toULongLong(&parsed);
    if (!id || !parsed)
        return;
    const auto match = std::find_if(snapshots_.begin(), snapshots_.end(), [&](const auto& snapshot) {
        return snapshot.job_id == *id && snapshot.generation == value && CanCancel(snapshot.phase);
    });
    if (match != snapshots_.end())
        emit cancelRequested(*id, value);
}

void RecordingClipAdapter::openCompleted(const QString& asset_id) {
    const auto id = engine::FromUuid<engine::MediaId>(asset_id.toStdString());
    if (!id)
        return;
    const auto match = std::find_if(snapshots_.begin(), snapshots_.end(), [&](const auto& snapshot) {
        return snapshot.phase == RecordingClipPhase::Completed && snapshot.asset_id == id && snapshot.completed &&
               snapshot.completed->succeeded;
    });
    if (match != snapshots_.end())
        emit openCompletedRequested(*id);
}

QVariantList RecordingClipAdapter::diagnosticResources(std::optional<uint64_t> recording_reserve) const {
    QVariantList resources;
    if (recording_reserve) {
        resources.push_back(
            QVariantMap{{QStringLiteral("identity"), tr("Recording storage")},
                        {QStringLiteral("metrics"),
                         QVariantList{QVariantMap{{QStringLiteral("label"), tr("Enforced recording reserve (bytes)")},
                                                  {QStringLiteral("value"), QString::number(*recording_reserve)},
                                                  {QStringLiteral("available"), true},
                                                  {QStringLiteral("reason"), QString()}}}}});
    }
    for (const auto& value : jobs_) {
        const auto job = value.toMap();
        const auto metric = [&](QString label, const QString& key) {
            return QVariantMap{{QStringLiteral("label"), std::move(label)},
                               {QStringLiteral("value"), job.value(key)},
                               {QStringLiteral("available"), true},
                               {QStringLiteral("reason"), QString()}};
        };
        resources.push_back(
            QVariantMap{{QStringLiteral("identity"), job.value(QStringLiteral("jobId"))},
                        {QStringLiteral("metrics"),
                         QVariantList{metric(tr("Job state"), QStringLiteral("phaseText")),
                                      metric(tr("Source bytes read"), QStringLiteral("sourceBytesRead")),
                                      metric(tr("Staging bytes written"), QStringLiteral("stagingBytesWritten")),
                                      metric(tr("Clip staging reserve (bytes)"), QStringLiteral("reservedBytes"))}}});
    }
    return resources;
}
} // namespace exosnap::quick
