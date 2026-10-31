#include "RecordingReviewAdapter.h"
#include "RecordViewModelAdapter.h"

#include <QTimer>
#include <algorithm>
#include <cmath>

namespace exosnap::quick {

RecordingReviewAdapter::RecordingReviewAdapter(QObject* parent) : QObject(parent) {
    session_.setMonitoringControlsInputMix(true);
    player_.setSession(&session_);
    const auto schedule = [this] { QTimer::singleShot(0, this, [this] { initializeSelection(); }); };
    connect(&player_, &EditPlayerAdapter::monitoringChanged, this, schedule);
    connect(&player_, &EditPlayerAdapter::previewChanged, this, schedule);
    connect(&player_, &EditPlayerAdapter::transportChanged, this, schedule);
    connect(&player_, &EditPlayerAdapter::clipOpenChanged, this, schedule);
}

void RecordingReviewAdapter::initializeSelection() {
    if (!active_ || !player_.clipOpen() || player_.transportPending())
        return;
    QStringList available;
    QStringList selected;
    QString preferred;
    for (const auto& value : player_.monitoringTracks()) {
        const auto track = value.toMap();
        if (!track.value("available").toBool())
            continue;
        const auto id = track.value("id").toString();
        available.push_back(id);
        if (preferred.isEmpty() && track.value("provenanceVerified").toBool() &&
            track.value("role").toString().compare(QStringLiteral("mix"), Qt::CaseInsensitive) == 0)
            preferred = id;
        if (track.value("selected").toBool())
            selected.push_back(id);
    }
    if (available.isEmpty() || available == initialized_tracks_)
        return;
    initialized_tracks_ = available;
    // Review auditions one stream, so overlapping recording tracks never sum implicitly.
    if (selected.size() != 1)
        selectTrack(!selected.isEmpty() ? selected.front() : !preferred.isEmpty() ? preferred : available.front());
}

void RecordingReviewAdapter::open(const EditContext& context) {
    close();
    active_ = true;
    recording_identity_ = RecordViewModelAdapter::recordingIdentity(context.output_path);
    edit_error_.clear();
    emit editErrorChanged();
    initialized_tracks_.clear();
    session_.setEditContext(context);
    emit activeChanged();
}

void RecordingReviewAdapter::close() {
    suspend();
    session_.close();
    initialized_tracks_.clear();
    recording_identity_.clear();
    if (active_) {
        active_ = false;
        emit activeChanged();
    }
}

void RecordingReviewAdapter::suspend() {
    player_.setPlaying(false);
    player_.endScrub();
    player_.setSurfaceVisible(false);
}

void RecordingReviewAdapter::completeEditRequest(const QString& identity, bool selected) {
    if (!active_ || identity != recording_identity_)
        return;
    if (selected) {
        close();
        return;
    }
    suspend();
    edit_error_ = tr("The recording is no longer available in Recordings.");
    emit editErrorChanged();
}

void RecordingReviewAdapter::selectTrack(const QString& id) {
    if (!active_ || !player_.clipOpen() || player_.transportPending())
        return;
    for (const auto& value : player_.monitoringTracks()) {
        const auto track = value.toMap();
        if (track.value("id").toString() == id && track.value("available").toBool()) {
            // Legacy metadata requires confirmation even for one non-overlapping stream.
            player_.confirmMonitoring({id}, !track.value("provenanceVerified").toBool());
            return;
        }
    }
}

void RecordingReviewAdapter::setMuted(bool muted) {
    if (muted_ == muted)
        return;
    muted_ = muted;
    player_.setVolume(muted_ ? 0.0 : volume_);
    emit audioChanged();
}

void RecordingReviewAdapter::setVolume(double volume) {
    if (!std::isfinite(volume))
        return;
    volume = std::clamp(volume, 0.0, 1.0);
    if (volume_ == volume)
        return;
    volume_ = volume;
    player_.setVolume(muted_ ? 0.0 : volume_);
    emit audioChanged();
}

} // namespace exosnap::quick
