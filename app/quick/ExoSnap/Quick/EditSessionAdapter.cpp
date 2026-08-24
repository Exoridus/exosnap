#include "EditSessionAdapter.h"

#include "diagnostics/AppLog.h"
#include "models/EditTimelineModel.h"
#include "models/MarkerSidecar.h"

#include <QFileInfo>
#include <QPointer>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>

namespace exosnap::quick {
namespace {

const QString kEmptyValue = QStringLiteral("\xe2\x80\x94");

QVariantMap factRow(const QString& label, const QString& value) {
    QVariantMap row;
    row.insert(QStringLiteral("label"), label);
    // The rail renders an unset fact as the app-wide em dash rather than as an
    // empty cell, so an unknown value still reads as a value.
    row.insert(QStringLiteral("value"), value.isEmpty() ? kEmptyValue : value);
    return row;
}

} // namespace

int64_t SnapTrimBoundaryUs(int64_t requested_us, const std::vector<int64_t>& keyframes_us,
                           const std::vector<RecordingMarker>& markers) {
    int64_t snapped = requested_us;
    if (!keyframes_us.empty()) {
        auto it = std::upper_bound(keyframes_us.begin(), keyframes_us.end(), snapped);
        if (it != keyframes_us.begin())
            --it;
        snapped = *it;
    }
    for (const auto& marker : markers) {
        const auto marker_us = static_cast<int64_t>(marker.time_ms) * 1000LL;
        if (std::abs(marker_us - snapped) <= kMarkerSnapWindowUs)
            return marker_us;
    }
    return snapped;
}

EditSessionAdapter::EditSessionAdapter(QObject* parent) : QObject(parent) {
    // One reader at a time: a second clip supersedes the first, and two parallel
    // container index reads on the same disk are slower than one.
    keyframe_pool_.setMaxThreadCount(1);
}

EditSessionAdapter::~EditSessionAdapter() = default;

void EditSessionAdapter::setEditContext(const EditContext& context) {
    context_ = context;
    ++clip_generation_;

    const qint64 duration =
        context_.duration_seconds > 0.0 ? static_cast<qint64>(std::llround(context_.duration_seconds * 1000.0)) : 0;
    const bool duration_changed = duration != duration_ms_;
    duration_ms_ = duration;

    keyframe_timestamps_.clear();
    trim_snap_ready_ = false;
    trim_start_us_ = exosnap::engine::TrimRange::kNoTimestamp;
    trim_end_us_ = exosnap::engine::TrimRange::kNoTimestamp;
    position_ms_ = 0;

    applyReport(context_);
    rebuildFacts();
    loadMarkers();

    const bool was_open = open_;
    open_ = !context_.mkv_master_path.isEmpty();

    emit clipChanged();
    if (duration_changed)
        emit durationChanged();
    emit trimChanged();
    emit trimSnapReadyChanged();
    emit positionChanged();
    emit unsavedEditsChanged();
    if (was_open != open_)
        emit openChanged();

    if (open_) {
        emit clipOpened(context_.mkv_master_path, duration_ms_);
        startKeyframeScan();
    } else {
        emit clipClosed();
    }
}

const EditContext& EditSessionAdapter::editContext() const noexcept {
    return context_;
}

bool EditSessionAdapter::open() const noexcept {
    return open_;
}

QString EditSessionAdapter::clipTitle() const {
    const QString& path = context_.output_path;
    const int separator = std::max(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
    return separator >= 0 ? path.mid(separator + 1) : path;
}

const QString& EditSessionAdapter::clipPath() const noexcept {
    return context_.output_path;
}

QString EditSessionAdapter::playerMetaText() const {
    QStringList parts;
    for (const QString& part : {context_.resolution, context_.fps, context_.container}) {
        if (!part.isEmpty())
            parts.append(part);
    }
    return parts.join(QStringLiteral("  "));
}

const QVariantList& EditSessionAdapter::facts() const noexcept {
    return facts_;
}

qint64 EditSessionAdapter::durationMs() const noexcept {
    return duration_ms_;
}

qint64 EditSessionAdapter::trimStartMs() const noexcept {
    return trim_start_us_ != exosnap::engine::TrimRange::kNoTimestamp ? trim_start_us_ / 1000 : 0;
}

qint64 EditSessionAdapter::trimEndMs() const noexcept {
    return trim_end_us_ != exosnap::engine::TrimRange::kNoTimestamp ? trim_end_us_ / 1000 : duration_ms_;
}

bool EditSessionAdapter::trimmed() const noexcept {
    return duration_ms_ > 0 && (trim_start_us_ != exosnap::engine::TrimRange::kNoTimestamp ||
                                trim_end_us_ != exosnap::engine::TrimRange::kNoTimestamp);
}

bool EditSessionAdapter::trimSnapReady() const noexcept {
    return trim_snap_ready_;
}

qint64 EditSessionAdapter::positionMs() const noexcept {
    return position_ms_;
}

int64_t EditSessionAdapter::trimStartUs() const noexcept {
    return trim_start_us_;
}

int64_t EditSessionAdapter::trimEndUs() const noexcept {
    return trim_end_us_;
}

const std::vector<RecordingMarker>& EditSessionAdapter::markers() const noexcept {
    return markers_;
}

const std::vector<int64_t>& EditSessionAdapter::keyframeTimestamps() const noexcept {
    return keyframe_timestamps_;
}

int EditSessionAdapter::reportSeverityValue() const noexcept {
    return static_cast<int>(report_severity_);
}

const QString& EditSessionAdapter::reportLabel() const noexcept {
    return report_label_;
}

QString EditSessionAdapter::reportTooltip() const {
    return QStringLiteral("%1\n%2\n%3").arg(report_drops_text_, report_drift_text_, report_health_text_);
}

bool EditSessionAdapter::exportRunning() const noexcept {
    return export_running_;
}

void EditSessionAdapter::setExportRunning(bool running) {
    if (export_running_ == running)
        return;
    export_running_ = running;
    emit exportRunningChanged();
}

bool EditSessionAdapter::hasUnsavedEdits() const {
    return trimmed() || !markers_.empty();
}

void EditSessionAdapter::requestTrim(qint64 start_ms, qint64 end_ms) {
    if (duration_ms_ <= 0)
        return;

    // Clamp first (handles never cross, minimum gap holds), then snap. Doing it
    // the other way round would let a snap push a handle past its neighbour.
    const qint64 clamped_end = ClampTrimEndMs(end_ms, start_ms, duration_ms_);
    const qint64 clamped_start = ClampTrimStartMs(start_ms, clamped_end);

    const int64_t start_us = clamped_start <= 0
                                 ? exosnap::engine::TrimRange::kNoTimestamp
                                 : SnapTrimBoundaryUs(clamped_start * 1000, keyframe_timestamps_, markers_);
    const int64_t end_us = clamped_end >= duration_ms_
                               ? exosnap::engine::TrimRange::kNoTimestamp
                               : SnapTrimBoundaryUs(clamped_end * 1000, keyframe_timestamps_, markers_);

    setTrimUs(start_us, end_us);

    // Show the frame at the boundary that actually moved. A drag of the in-point
    // is answered by the in-point; a drag of the out-point by the out-point.
    const int64_t shown_us = clamped_start <= 0 ? end_us : start_us;
    if (shown_us != exosnap::engine::TrimRange::kNoTimestamp)
        emit seekRequested(shown_us / 1000);
}

void EditSessionAdapter::requestSeek(qint64 position_ms) {
    const qint64 clamped = ClampPlayheadMs(position_ms, duration_ms_);
    if (clamped != position_ms_) {
        position_ms_ = clamped;
        emit positionChanged();
    }
    emit seekRequested(clamped);
}

void EditSessionAdapter::setPositionMs(qint64 position_ms) {
    const qint64 clamped = ClampPlayheadMs(position_ms, duration_ms_);
    if (clamped == position_ms_)
        return;
    position_ms_ = clamped;
    emit positionChanged();
}

// Closing the Edit surface is a session close, not a view change.
//
// The surface is a QML Loader over adapters that live for the life of the
// process, so unloading it destroys items and nothing else: before this, close()
// emitted closeRequested() alone, the overlay disappeared, and the clip stayed
// open behind it -- the player kept its decoder session (and its WASAPI
// renderer), the timeline's thumbnail worker kept the container open, and the
// recording could not be moved or deleted until ExoSnap exited. A second Edit
// session then started on top of the first one's leftovers.
//
// clipClosed() is what the player and the timeline hang their teardown off, so
// it is emitted here as well as from setEditContext(), and always before
// closeRequested(): the resources are released first, the surface goes second.
// Idempotent -- a second close on an already-empty session only repeats the
// request to dismiss the surface.
void EditSessionAdapter::close() {
    // A fixture context carries a duration but no master path, and must close
    // just as completely as a real clip -- hence not `open_` alone.
    const bool had_clip = open_ || duration_ms_ > 0 || !context_.output_path.isEmpty();
    if (!had_clip) {
        emit closeRequested();
        return;
    }

    const bool was_open = open_;
    // A keyframe scan still running for this clip belongs to a generation nobody
    // is showing any more; advancing the counter drops its result on arrival.
    ++clip_generation_;
    context_ = EditContext{};
    keyframe_timestamps_.clear();
    trim_snap_ready_ = false;
    trim_start_us_ = exosnap::engine::TrimRange::kNoTimestamp;
    trim_end_us_ = exosnap::engine::TrimRange::kNoTimestamp;
    duration_ms_ = 0;
    position_ms_ = 0;
    open_ = false;

    applyReport(context_);
    rebuildFacts();
    loadMarkers();

    emit clipChanged();
    emit durationChanged();
    emit trimChanged();
    emit trimSnapReadyChanged();
    emit positionChanged();
    emit unsavedEditsChanged();
    if (was_open)
        emit openChanged();

    emit clipClosed();
    emit closeRequested();
}

QString EditSessionAdapter::formatTimestamp(qint64 position_ms) const {
    return FormatTimelineTimestamp(position_ms, duration_ms_);
}

QString EditSessionAdapter::formatClock(qint64 position_ms) const {
    return FormatTimelineClock(position_ms, duration_ms_);
}

qint64 EditSessionAdapter::clampTrimStartMs(qint64 requested_ms, qint64 trim_end_ms) const {
    return ClampTrimStartMs(requested_ms, trim_end_ms);
}

qint64 EditSessionAdapter::clampTrimEndMs(qint64 requested_ms, qint64 trim_start_ms) const {
    return ClampTrimEndMs(requested_ms, trim_start_ms, duration_ms_);
}

void EditSessionAdapter::setKeyframeTimestampsForTest(std::vector<int64_t> keyframes_us) {
    keyframe_timestamps_ = std::move(keyframes_us);
    std::sort(keyframe_timestamps_.begin(), keyframe_timestamps_.end());
    if (!trim_snap_ready_) {
        trim_snap_ready_ = true;
        emit trimSnapReadyChanged();
    }
}

void EditSessionAdapter::setTrimUs(int64_t start_us, int64_t end_us) {
    if (trim_start_us_ == start_us && trim_end_us_ == end_us)
        return;
    const bool had_edits = hasUnsavedEdits();
    trim_start_us_ = start_us;
    trim_end_us_ = end_us;
    emit trimChanged();
    if (had_edits != hasUnsavedEdits())
        emit unsavedEditsChanged();
}

void EditSessionAdapter::applyReport(const EditContext& context) {
    const auto& snapshot = context.completed_snapshot;
    const bool has_snapshot = snapshot.valid || snapshot.session_generation > 0;

    // REAL drops only (encoder backpressure plus frame-processing failures) --
    // not deliberate CFR pacing/coalescing, which is intentional frame
    // selection, not a drop. frames_dropped_problem() is the shared definition.
    const uint64_t dropped = snapshot.capture.frames_dropped_problem();
    const uint64_t total = snapshot.capture.frames_emitted + dropped;
    if (has_snapshot && total > 0) {
        const double percent = 100.0 * static_cast<double>(dropped) / static_cast<double>(total);
        report_drops_text_ = QStringLiteral("Frame drops: %1%").arg(percent, 0, 'f', 1);
    } else {
        report_drops_text_ = QStringLiteral("Frame drops: %1").arg(kEmptyValue);
    }

    if (context.av_drift_available) {
        report_drift_text_ =
            QStringLiteral("Peak A/V drift: \xc2\xb1%1\xc2\xa0ms").arg(context.peak_av_drift_ms, 0, 'f', 0);
    } else {
        report_drift_text_ = QStringLiteral("Peak A/V drift: %1").arg(kEmptyValue);
    }

    // The label is the verdict itself in every case, not only in the two that go
    // wrong. It used to be empty unless the pipeline reported Warning or
    // Critical, so the header element showed the word "Report" the rest of the
    // time — one element switching between reading as an ACTION ("open the
    // report") and as a STATUS ("this recording has a warning"), which is
    // exactly the ambiguity a header must not carry. Now it always states a
    // verdict and the severity decides only how loudly.
    report_severity_ = ReportSeverity::Neutral;
    if (has_snapshot) {
        QString health = QStringLiteral("Unknown");
        switch (snapshot.health) {
        case exosnap::engine::PipelineHealth::Good:
            health = QStringLiteral("Good");
            break;
        case exosnap::engine::PipelineHealth::Warning:
            health = QStringLiteral("Warning");
            report_severity_ = ReportSeverity::Warning;
            break;
        case exosnap::engine::PipelineHealth::Critical:
            health = QStringLiteral("Critical");
            report_severity_ = ReportSeverity::Critical;
            break;
        case exosnap::engine::PipelineHealth::Unavailable:
            health = QStringLiteral("Unavailable");
            break;
        default:
            break;
        }
        report_label_ = health;
        report_health_text_ = QStringLiteral("Pipeline health: %1").arg(health);
    } else {
        report_label_ = kEmptyValue;
        report_health_text_ = QStringLiteral("Pipeline health: %1").arg(kEmptyValue);
    }
    emit reportChanged();
}

void EditSessionAdapter::rebuildFacts() {
    facts_.clear();
    facts_.append(factRow(QStringLiteral("Duration"), context_.duration));
    facts_.append(factRow(QStringLiteral("Size"), context_.size));
    facts_.append(factRow(QStringLiteral("Resolution"), context_.resolution));
    facts_.append(factRow(QStringLiteral("Frame rate"), context_.fps));
    facts_.append(factRow(QStringLiteral("Video"), context_.video_codec));
    facts_.append(factRow(QStringLiteral("Audio"), context_.audio_codec));
    facts_.append(factRow(QStringLiteral("Container"), context_.container));
}

void EditSessionAdapter::loadMarkers() {
    markers_.clear();
    // Canonical source: the "<media>.markers.json" sidecar RecordingCoordinator
    // writes. Once it exists it is authoritative; otherwise fall back to the
    // markers the result carried.
    if (!context_.marker_sidecar_path.isEmpty()) {
        const std::filesystem::path sidecar(context_.marker_sidecar_path.toStdWString());
        std::error_code ec;
        if (std::filesystem::exists(sidecar, ec)) {
            int skipped = 0;
            markers_ = ReadMarkerSidecar(sidecar, &skipped);
            // QCR-207: a dropped marker is a silent difference between the file
            // and the timeline, so it is stated once here rather than nowhere.
            // No separate marker-error surface — the sidecar is a companion
            // file, and losing an entry costs a bookmark, not the recording.
            if (skipped > 0) {
                diagnostics::AppLog::warning(QStringLiteral("edit"),
                                             QStringLiteral("Skipped %1 marker(s) with an unusable time in %2")
                                                 .arg(skipped)
                                                 .arg(context_.marker_sidecar_path));
            }
            return;
        }
    }
    markers_ = context_.markers;
}

// Reading a container's full index is proportional to the recording's length,
// and the Widgets surface did it inline in setEditContext -- a long recording
// froze the GUI thread on open. Off-thread here; a trim released before it lands
// still snaps to markers, and re-snaps as soon as the table arrives.
void EditSessionAdapter::startKeyframeScan() {
    const quint64 generation = clip_generation_;
    const std::filesystem::path master(context_.mkv_master_path.toStdWString());
    QPointer<EditSessionAdapter> guard(this);
    keyframe_pool_.start([this, guard, generation, master]() {
        std::vector<int64_t> keyframes = exosnap::engine::ExtractKeyframeTimestamps(master);
        QMetaObject::invokeMethod(
            this,
            [this, guard, generation, keyframes = std::move(keyframes)]() mutable {
                if (guard.isNull() || generation != clip_generation_)
                    return;
                keyframe_timestamps_ = std::move(keyframes);
                std::sort(keyframe_timestamps_.begin(), keyframe_timestamps_.end());
                trim_snap_ready_ = true;
                emit trimSnapReadyChanged();
            },
            Qt::QueuedConnection);
    });
}

} // namespace exosnap::quick
