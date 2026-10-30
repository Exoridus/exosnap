#include "EditPlayerAdapter.h"
#include "EditRenderText.h"
#include <QCoreApplication>
#include <QDateTime>

#include "EditPlayerWorker.h"
#include "EditSessionAdapter.h"
#include "ExoEditPlayerItem.h"

#include <QGuiApplication>
#include <QQuickWindow>
#include <QScreen>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

namespace exosnap::quick {
namespace {

// Fallback tick, used only until a clip is open (or when its container declares
// no usable frame rate).
constexpr int kFallbackPreviewTickMs = 16;

} // namespace

int EditPreviewTickMsFor(double clip_fps, double screen_hz) noexcept {
    double hz = clip_fps;
    if (!(hz > 0.0))
        return kFallbackPreviewTickMs;
    if (screen_hz > 0.0)
        hz = std::min(hz, screen_hz);
    // Round down so the timer never lands just past a frame boundary, and never
    // go below 1 ms (a QTimer cannot honour 0 as "once per frame").
    return std::max(1, static_cast<int>(std::floor(1000.0 / hz)));
}

void EditPlayerFrameSink::attach(ExoEditPlayerItem* item) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (item_ != item)
        scene_graph_consumption_.reset();
    item_ = item;
}

void EditPlayerFrameSink::detach(const ExoEditPlayerItem* item) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (item_ == item) {
        item_ = nullptr;
        scene_graph_consumption_.reset();
    }
}

void EditPlayerFrameSink::deliver(exosnap::engine::RawDecodedVideoFrame frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (item_ != nullptr)
        item_->presentFrame(std::move(frame));
}

void EditPlayerFrameSink::deliver(exosnap::engine::RawDecodedVideoFrame frame, uint64_t generation,
                                  int64_t timeline_offset_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!item_ || generation != generation_)
        return;
    if ((timeline_offset_us > 0 && frame.pts_us > (std::numeric_limits<int64_t>::max)() - timeline_offset_us) ||
        (timeline_offset_us < 0 && frame.pts_us < (std::numeric_limits<int64_t>::min)() - timeline_offset_us))
        return;
    const int64_t timeline_time_us = frame.pts_us + timeline_offset_us;
    const auto stamp = revision_ ? std::optional<EditFramePresentation>{{generation, *revision_}} : std::nullopt;
    item_->presentFrame(std::move(frame), timeline_time_us, stamp);
    output_revision_ = revision_;
}

void EditPlayerFrameSink::publishClock(int64_t media_time_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (item_ != nullptr)
        item_->setClockUs(media_time_us);
}

uint64_t EditPlayerFrameSink::invalidate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (item_)
        item_->supersedePendingFrame();
    revision_.reset();
    return ++generation_;
}

void EditPlayerFrameSink::setRevision(uint64_t generation, edit::Revision revision) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == generation_)
        revision_ = revision;
}

std::optional<edit::Revision> EditPlayerFrameSink::outputRevision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return output_revision_;
}

std::optional<EditPlayerFrameSink::SceneGraphConsumption> EditPlayerFrameSink::sceneGraphConsumption() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return scene_graph_consumption_;
}

bool EditPlayerFrameSink::recordSceneGraphConsumption(uint64_t generation, edit::Revision revision, int64_t time_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!item_ || generation != generation_)
        return false;
    scene_graph_consumption_ = SceneGraphConsumption{generation, revision, time_us};
    return true;
}

void EditPlayerFrameSink::clearSceneGraphConsumption() {
    std::lock_guard<std::mutex> lock(mutex_);
    scene_graph_consumption_.reset();
}
void EditPlayerFrameSink::deliverTimeline(exosnap::engine::TimelineVideoFrame frame, uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (item_ && generation == generation_) {
        const auto stamp = revision_ ? std::optional<EditFramePresentation>{{generation, *revision_}} : std::nullopt;
        item_->presentTimelineFrame(std::move(frame), stamp);
        output_revision_ = revision_;
    }
}

void EditPlayerFrameSink::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    output_revision_.reset();
    scene_graph_consumption_.reset();
    if (item_ != nullptr)
        item_->clearFrame();
}

EditPlayerAdapter::EditPlayerAdapter(QObject* parent)
    : QObject(parent), placeholder_text_(QCoreApplication::translate("EditPlayerAdapter", "Preview unavailable")) {
    connect(this, &EditPlayerAdapter::boundsChanged, this, [this] {
        if (!session_)
            return;
        const auto region = bounds_.loopRegion();
        if (bounds_.loopEnabled() && region)
            session_->setLoopSnapBounds(region->start_us, region->end_us);
        else
            session_->setLoopSnapBounds(-1, -1);
    });
    qRegisterMetaType<engine::MonitorSelectionSnapshot>();
    worker_ = new EditPlayerWorker(sink_);
    worker_->moveToThread(&worker_thread_);
    connect(&worker_thread_, &QThread::finished, worker_, &QObject::deleteLater);

    connect(worker_, &EditPlayerWorker::openFinishedForCommand, this,
            [this](qulonglong command, qulonglong generation, bool opened, const QString& error) {
                if (generation != request_generation_ || command != transport_command_)
                    return;
                if (clip_open_ != opened) {
                    clip_open_ = opened;
                    emit clipOpenChanged();
                }
                // An open timeline can still report an unavailable segment; that is a
                // degraded preview, not a ready one.
                const bool failed = !opened || !error.isEmpty();
                // A compatible publication acknowledges controls, not a new successful render.
                const bool retained_error =
                    !failed && !seek_pending_ && (preview_state_ == Degraded || preview_state_ == Failed);
                if (!retained_error)
                    setPlaceholderText(!failed ? QString()
                                               : (error.isEmpty() ? QCoreApplication::translate("EditPlayerAdapter",
                                                                                                "Preview unavailable")
                                                                  : TranslateEditRenderReason(error)));
                if (failed) {
                    if (!opened)
                        setPlaying(false);
                    seek_pending_ = false;
                    setPreviewState(duration_ms_ <= 0 ? Empty : presented_time_us_ ? Degraded : Failed);
                    emit previewChanged();
                }
            });
    connect(worker_, &EditPlayerWorker::positionAdvancedForCommand, this,
            [this](qulonglong command, qulonglong generation, qint64 position_us) {
                if (command != transport_command_ || generation != request_generation_)
                    return;
                if (session_ != nullptr)
                    session_->setPositionUs(position_us);
                if (!seek_pending_ && requested_time_us_ != position_us) {
                    requested_time_us_ = position_us;
                    emit previewChanged();
                }
            });
    connect(worker_, &EditPlayerWorker::transportStateChanged, this,
            [this](qulonglong command, qulonglong generation, bool playing) {
                if (command != transport_command_ || generation != request_generation_)
                    return;
                if (playing_ == playing)
                    return;
                playing_ = playing;
                emit playingChanged();
            });
    connect(worker_, &EditPlayerWorker::transportCompleted, this,
            [this](qulonglong command, qulonglong generation, bool playing) {
                if (command != transport_command_ || generation != request_generation_)
                    return;
                transport_pending_ = false;
                if (!scrubbing_)
                    desired_playing_ = playing;
                emit transportChanged();
            });
    connect(worker_, &EditPlayerWorker::transportEnded, this, [this](qulonglong command, qulonglong generation) {
        if (command != transport_command_ || generation != request_generation_)
            return;
        desired_playing_ = false;
        bounds_.completeOnce();
        emit boundsChanged();
        transport_pending_ = false;
        emit transportChanged();
        if (!playing_)
            return;
        playing_ = false;
        emit playingChanged();
    });

    connect(worker_, &EditPlayerWorker::monitoringStateChangedForGeneration, this,
            [this](qulonglong generation, QVariantList tracks, bool required) {
                if (generation == request_generation_)
                    setMonitoringState(std::move(tracks), required);
            });
    connect(worker_, &EditPlayerWorker::monitoringRejectedForCommand, this,
            [this](qulonglong command, qulonglong generation, const QString& error) {
                if (command != transport_command_ || generation != request_generation_)
                    return;
                setPlaying(false);
                setPlaceholderText(error);
            });
    connect(worker_, &EditPlayerWorker::monitorSelectionConfirmed, this,
            [this](qulonglong request, qulonglong asset, engine::MonitorSelectionSnapshot selection) {
                if (session_ && request == monitoring_request_)
                    session_->setAssetMonitorSelection(asset, std::move(selection));
            });

    worker_thread_.setObjectName(QStringLiteral("exosnap-edit-player"));
    worker_thread_.start();
}

EditPlayerAdapter::~EditPlayerAdapter() {
    opening_cancelled_->store(true);
    current_transport_->store(++transport_command_);
    // Stop delivery first: the decode thread must not reach a half-destroyed
    // item, and close() is what joins that thread.
    sink_->detach(item_.data());
    QMetaObject::invokeMethod(worker_, "close", Qt::BlockingQueuedConnection);
    worker_thread_.quit();
    worker_thread_.wait();
}

QVariantMap EditPlayerAdapter::measurementSnapshot() {
    Q_ASSERT(QThread::currentThread() == thread());
    using Clock = std::chrono::steady_clock;
    constexpr auto cadence = std::chrono::milliseconds(250);
    const auto now = Clock::now();
    const bool available = worker_ && worker_thread_.isRunning();
    if (available && !measurement_pending_ &&
        (!measurement_requested_at_ || now - *measurement_requested_at_ >= cadence)) {
        measurement_pending_ = true;
        measurement_requested_at_ = now;
        const auto request = ++measurement_request_id_;
        const auto session = measurement_session_;
        const auto generation = request_generation_;
        const bool queued = QMetaObject::invokeMethod(
            worker_,
            [this, worker = worker_, request, session, generation] {
                auto values = worker->performanceSnapshot();
                const auto captured = Clock::now();
                auto utc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                // Destruction joins this worker before the adapter's QObject lifetime ends.
                QMetaObject::invokeMethod(
                    this,
                    [this, request, session, generation, captured, utc = std::move(utc),
                     values = std::move(values)]() mutable {
                        if (request != measurement_request_id_)
                            return;
                        measurement_pending_ = false;
                        if (session != measurement_session_ || generation != request_generation_ ||
                            values.value("requestGeneration").toULongLong() != generation)
                            return;
                        measurement_sample_ = MeasurementSample{std::move(values),        captured, std::move(utc),
                                                                ++measurement_sample_id_, session,  generation};
                    },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
        if (!queued)
            measurement_pending_ = false;
    }
    const auto* sample = measurement_sample_ ? &*measurement_sample_ : nullptr;
    const auto age = sample ? std::chrono::duration_cast<std::chrono::milliseconds>(now - sample->captured).count() : 0;
    const bool current = sample && sample->session == measurement_session_ &&
                         sample->generation == request_generation_ && now - sample->captured <= cadence;
    const auto status = !available             ? QStringLiteral("unavailable")
                        : current              ? QStringLiteral("available")
                        : sample               ? QStringLiteral("stale")
                        : measurement_pending_ ? QStringLiteral("pending")
                                               : QStringLiteral("unavailable");
    return {{"status", status},
            {"pending", measurement_pending_},
            {"cadenceMs", 250},
            {"ageClock", QStringLiteral("steady_clock")},
            {"requestId", QVariant::fromValue(measurement_request_id_)},
            {"currentSessionId", QVariant::fromValue(measurement_session_)},
            {"currentGeneration", QVariant::fromValue(request_generation_)},
            {"sampleId", sample ? QVariant::fromValue(sample->id) : QVariant{}},
            {"sessionId", sample ? QVariant::fromValue(sample->session) : QVariant{}},
            {"generation", sample ? QVariant::fromValue(sample->generation) : QVariant{}},
            {"capturedUtc", sample ? QVariant(sample->captured_utc) : QVariant{}},
            {"ageMs", sample ? QVariant::fromValue(age) : QVariant{}},
            {"values", sample ? sample->values : QVariantMap{}}};
}

void EditPlayerAdapter::setSession(EditSessionAdapter* session) {
    if (session_ == session)
        return;
    pending_timeline_current_.reset();
    if (session_)
        disconnect(session_, nullptr, this, nullptr);
    closeClip();
    session_ = session;
    if (session_ == nullptr)
        return;
    connect(session_, &QObject::destroyed, this, [this]() {
        session_ = nullptr;
        closeClip();
    });
    connect(session_, &EditSessionAdapter::workspaceChanged, this, &EditPlayerAdapter::submitTimeline);
    connect(session_, &EditSessionAdapter::sourceVerificationChanged, this, [this](qulonglong asset) {
        const bool unavailable = session_->sourceVerificationFailed(asset);
        if (unavailable == unavailable_sources_.contains(asset))
            return;
        if (unavailable)
            unavailable_sources_.insert(asset);
        else
            unavailable_sources_.erase(asset);
        timeline_revision_.reset();
        submitTimeline();
        if (unavailable)
            setPlaceholderText(session_->sourceVerificationReason(asset));
    });
    connect(session_, &EditSessionAdapter::clipClosed, this, &EditPlayerAdapter::closeClip);
    connect(session_, &EditSessionAdapter::seekRequestedUs, this, &EditPlayerAdapter::seekUs);
    connect(session_, &EditSessionAdapter::inspectionRequested, this, [this]() { setPlaying(false); });
    connect(session_, &EditSessionAdapter::auditionChanged, this, [this] {
        std::vector<uint64_t> tracks;
        for (const auto& id : session_->soloTrackIds())
            tracks.push_back(id.toULongLong());
        QMetaObject::invokeMethod(
            worker_,
            [worker = worker_, tracks = std::move(tracks)]() mutable { worker->setSoloTracks(std::move(tracks)); },
            Qt::QueuedConnection);
    });
    connect(session_, &EditSessionAdapter::editRangeCommitted, this, [this](qint64 start, qint64 end) {
        const auto previous = bounds_.loopRegion();
        bounds_.commitRange({start, end});
        if (previous == bounds_.loopRegion())
            return;
        emit boundsChanged();
        if (bounds_.repeating())
            applyBoundsChange();
    });
    connect(session_, &EditSessionAdapter::editRangeCleared, this, [this] {
        const auto previous = bounds_.loopRegion();
        bounds_.clearRange();
        if (previous == bounds_.loopRegion())
            return;
        emit boundsChanged();
        if (bounds_.repeating())
            applyBoundsChange();
    });
}

std::optional<edit::PlaybackInterval> EditPlayerAdapter::activeEditRange() const {
    if (!session_ || session_->selectionMode() != EditSessionAdapter::RangeSelection)
        return {};
    const auto range = session_->editRange();
    if (range.isEmpty())
        return {};
    return edit::PlaybackInterval{range.value(QStringLiteral("startUs")).toLongLong(),
                                  range.value(QStringLiteral("endUs")).toLongLong()};
}

QVariantMap EditPlayerAdapter::loopRegion() const {
    const auto region = bounds_.loopRegion();
    return region
               ? QVariantMap{{QStringLiteral("startUs"), region->start_us}, {QStringLiteral("endUs"), region->end_us}}
               : QVariantMap{};
}

void EditPlayerAdapter::setLoopEnabled(bool enabled) {
    if (session_)
        session_->cancelRangeGesture();
    if (enabled == bounds_.loopEnabled())
        return;
    bounds_.setLoop(enabled, activeEditRange());
    emit boundsChanged();
    if (!bounds_.onceActive())
        applyBoundsChange();
}

void EditPlayerAdapter::playRangeOnce() {
    const auto range = activeEditRange();
    if (!range || !bounds_.beginOnce(*range)) {
        setPlaceholderText(QCoreApplication::translate("EditPlayerAdapter", "Select a nonempty time range first."));
        return;
    }
    emit boundsChanged();
    desired_playing_ = true;
    if (session_)
        session_->setPositionUs(range->start_us);
    seekUs(range->start_us);
}

void EditPlayerAdapter::applyBoundsChange() {
    const auto region = bounds_.interval();
    if (desired_playing_ && !scrubbing_) {
        auto position = session_ ? session_->positionUs() : requested_time_us_;
        if (bounds_.repeating() && (position < region.start_us || position >= region.end_us))
            position = region.start_us;
        if (session_)
            session_->setPositionUs(position);
        seekUs(position);
    } else {
        QMetaObject::invokeMethod(
            worker_,
            [worker = worker_, region, repeat = bounds_.repeating()] { worker->setPlaybackBounds(region, repeat); },
            Qt::QueuedConnection);
    }
}

void EditPlayerAdapter::submitTimeline() {
    if (!session_)
        return;
    duration_ms_ = session_->durationMs();
    const auto& current = session_->workspace();
    const auto was_once = bounds_.onceActive();
    bounds_.setDuration(current.duration());
    if (was_once && !bounds_.onceActive())
        desired_playing_ = false;
    emit boundsChanged();
    if (timeline_revision_ == current.revision() && timeline_monitoring_revision_ == current.monitoringRevision())
        return;
    std::vector<engine::TimelineMediaSource> sources;
    for (const auto& asset : current.assets()) {
        if (unavailable_sources_.contains(asset.id))
            continue;
        const auto metadata = session_->sourceMetadata(asset.id);
        if (!metadata)
            continue;
        sources.push_back({asset.id, std::filesystem::path(asset.path), *metadata, asset.monitor_selection,
                           session_->sourceAccess(asset.id), session_->sourceProof(asset.id)});
    }
    auto snapshot = std::make_shared<EditPlaybackSnapshot>(current, std::move(sources), unavailable_sources_);
    const bool retains_media = published_snapshot_ && published_snapshot_->retainsMedia(*snapshot);
    published_snapshot_ = snapshot;
    timeline_revision_ = current.revision();
    timeline_monitoring_revision_ = current.monitoringRevision();
    transport_pending_ = true;
    const auto command = ++transport_command_;
    current_transport_->store(command);
    emit transportChanged();
    const auto generation = retains_media ? request_generation_ : sink_->invalidate();
    request_generation_ = generation;
    pending_seek_.reset();
    requested_time_us_ = current.playhead();
    seek_pending_ = !retains_media && current.duration() > 0;
    if (!retains_media || current.duration() <= 0)
        setPreviewState(!seek_pending_ ? Empty : presented_time_us_ ? SeekPending : Opening);
    emit previewChanged();
    if (pending_timeline_current_)
        pending_timeline_current_->store(false);
    const auto current_update = std::make_shared<std::atomic<bool>>(true);
    pending_timeline_current_ = current_update;
    const auto submitted = std::chrono::steady_clock::now();
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, snapshot = std::move(snapshot), position = requested_time_us_,
         wants_play = desired_playing_ && !scrubbing_, current_transport = current_transport_, generation, command,
         submitted, current_update, cancelled = opening_cancelled_, region = bounds_.interval(),
         repeat = bounds_.repeating()]() {
            if (!current_update->load())
                return;
            worker->recordQueueWait(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - submitted)
                    .count(),
                true);
            worker->setOpeningCancellation(cancelled);
            worker->setRequestGeneration(generation);
            worker->setTransportCommand(command);
            worker->setPlaybackBounds(region, repeat);
            worker->setTimeline(snapshot, position, [current_transport, command, wants_play]() {
                return current_transport->load() == command && wants_play;
            });
            worker->acknowledgeTransport();
        },
        Qt::QueuedConnection);
}

void EditPlayerAdapter::attachPlayerItem(ExoEditPlayerItem* item) {
    if (item_ && item_ != item) {
        disconnect(item_, nullptr, this, nullptr);
        sink_->detach(item_);
    }
    item_ = item;
    sink_->attach(item);
    connect(item, &ExoEditPlayerItem::framePresented, this, [this](qint64 time_us) {
        presented_time_us_ = time_us;
        seek_pending_ = false;
        setPlaceholderText({});
        setPreviewState(Ready);
        emit previewChanged();
    });
    connect(item, &ExoEditPlayerItem::frameConsumedWithRevision, this,
            [this](qint64 time_us, quint64 generation, quint64 revision) {
                if (sink_->recordSceneGraphConsumption(generation, revision, time_us))
                    emit previewChanged();
            });
    connect(item, &ExoEditPlayerItem::hasFrameChanged, this, [this]() {
        if (!item_ || item_->hasFrame())
            return;
        presented_time_us_.reset();
        sink_->clearSceneGraphConsumption();
        setPreviewState(duration_ms_ > 0 ? Opening : Empty);
        emit previewChanged();
    });
    connect(item, &ExoEditPlayerItem::errorTextChanged, this, [this]() {
        if (item_ && !item_->errorText().isEmpty()) {
            seek_pending_ = false;
            setPlaceholderText(item_->errorText());
            setPreviewState(presented_time_us_ ? Degraded : Failed);
            emit previewChanged();
        }
    });
    if (session_ && !playing_)
        seekUs(session_->positionUs());
}

void EditPlayerAdapter::detachPlayerItem(ExoEditPlayerItem* item) {
    sink_->detach(item);
    if (item_ == item) {
        disconnect(item, nullptr, this, nullptr);
        item_.clear();
        presented_time_us_.reset();
        sink_->clearSceneGraphConsumption();
        setPreviewState(duration_ms_ > 0 ? Opening : Empty);
        emit previewChanged();
    }
}

QVariant EditPlayerAdapter::presentedTimeUs() const {
    return presented_time_us_ ? QVariant::fromValue<qint64>(*presented_time_us_) : QVariant{};
}

void EditPlayerAdapter::setPreviewState(PreviewState state) {
    if (preview_state_ == state)
        return;
    preview_state_ = state;
    emit previewChanged();
}

bool EditPlayerAdapter::playing() const noexcept {
    return playing_;
}

bool EditPlayerAdapter::clipOpen() const noexcept {
    return clip_open_;
}

bool EditPlayerAdapter::scrubbing() const noexcept {
    return scrubbing_;
}

const QString& EditPlayerAdapter::placeholderText() const noexcept {
    return placeholder_text_;
}

bool EditPlayerAdapter::surfaceVisible() const noexcept {
    return surface_visible_;
}

void EditPlayerAdapter::setSurfaceVisible(bool visible) {
    if (surface_visible_ == visible)
        return;
    surface_visible_ = visible;
    emit surfaceVisibleChanged();
    if (!visible)
        setPlaying(false); // the existing pause intent, not a second one
}

void EditPlayerAdapter::setClipStateForTest(bool clip_open, qint64 duration_ms) {
    duration_ms_ = duration_ms;
    bounds_.setDuration(duration_ms * 1000);
    if (clip_open_ == clip_open)
        return;
    clip_open_ = clip_open;
    emit clipOpenChanged();
}

void EditPlayerAdapter::showStillForHarness(const QUrl& source, qint64 time_us, qint64 duration_ms) {
    setSession(nullptr);
    harness_still_source_ = source;
    requested_time_us_ = time_us;
    presented_time_us_.reset();
    seek_pending_ = false;
    setPlaceholderText({});
    setClipStateForTest(true, duration_ms);
    emit harnessStillSourceChanged();
    emit previewChanged();
}

void EditPlayerAdapter::applyPreviewStateForHarness(PreviewState state, const QString& reason) {
    if (session_ || playing_ || desired_playing_ ||
        (state != Opening && state != Buffering && state != Degraded && state != Failed))
        return;
    if ((state == Opening || state == Buffering) && !harness_still_source_.isEmpty()) {
        harness_still_source_.clear();
        emit harnessStillSourceChanged();
    }
    QString message = reason;
    if (message.isEmpty()) {
        if (state == Opening)
            message = QCoreApplication::translate("EditPlayerAdapter", "Opening clip\xe2\x80\xa6");
        else if (state == Buffering)
            message = QCoreApplication::translate("EditPlayer", "Preview pending");
        else
            message = QCoreApplication::translate("EditPlayerAdapter", "Preview unavailable");
    }
    setPlaceholderText(message);
    setPreviewState(state);
}

void EditPlayerAdapter::setLoopRangeForHarness(qint64 start_us, qint64 end_us) {
    if (session_ || playing_ || desired_playing_ || start_us < 0 || end_us <= start_us || end_us > duration_ms_ * 1000)
        return;
    const edit::PlaybackInterval range{start_us, end_us};
    bounds_.setLoop(true, range);
    bounds_.commitRange(range);
    emit boundsChanged();
}

void EditPlayerAdapter::setPlaceholderText(const QString& text) {
    if (placeholder_text_ == text)
        return;
    placeholder_text_ = text;
    emit placeholderTextChanged();
}

void EditPlayerAdapter::togglePlay() {
    setPlaying(!desired_playing_);
}

void EditPlayerAdapter::setPlaying(bool playing) {
    if (session_)
        session_->cancelRangeGesture();
    if (playing == desired_playing_)
        return;
    if (playing && duration_ms_ <= 0) {
        setPlaceholderText(QCoreApplication::translate("EditPlayerAdapter", "The timeline is empty."));
        return;
    }
    if (playing && !session_ && monitoring_confirmation_required_) {
        setPlaceholderText(QCoreApplication::translate("EditPlayerAdapter",
                                                       "Confirm the audio monitoring selection before playback."));
        return;
    }
    desired_playing_ = playing;
    if (playing) {
        const auto region = bounds_.interval();
        const auto position = session_ ? session_->positionUs() : requested_time_us_;
        if (position >= region.end_us || (bounds_.repeating() && position < region.start_us)) {
            if (session_)
                session_->setPositionUs(region.start_us);
            seekUs(region.start_us);
            return;
        }
    }
    dispatchTransport(!playing && (pending_seek_.has_value() || seek_dispatch_active_));
}

void EditPlayerAdapter::dispatchTransport(bool seek) {
    transport_pending_ = true;
    const auto command = ++transport_command_;
    current_transport_->store(command);
    const auto generation = request_generation_;
    const bool playing = desired_playing_ && !scrubbing_;
    const qint64 from = seek ? requested_time_us_ : session_ ? session_->positionUs() : 0;
    pending_seek_.reset();
    pending_timeline_current_.reset();
    const auto refresh = currentScreenRefreshHz();
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, current = current_transport_, command, generation, from, seek, playing, refresh,
         region = bounds_.interval(), repeat = bounds_.repeating()]() {
            if (current->load() != command)
                return;
            worker->setScreenRefreshHz(refresh);
            worker->setPlaybackBounds(region, repeat);
            worker->requestTransport(command, generation, from, seek, playing);
        },
        Qt::QueuedConnection);
    emit transportChanged();
}

void EditPlayerAdapter::confirmMonitoring(QStringList trackIds, bool overlapConfirmed) {
    setPlaying(false);
    const auto request = ++monitoring_request_;
    pending_timeline_current_.reset();
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, trackIds = std::move(trackIds), overlapConfirmed, request]() mutable {
            worker->confirmMonitoring(std::move(trackIds), overlapConfirmed, request);
        },
        Qt::QueuedConnection);
}

void EditPlayerAdapter::setMonitoringState(QVariantList tracks, bool confirmationRequired) {
    if (confirmationRequired && !session_)
        setPlaying(false);
    if (monitoring_tracks_ == tracks && monitoring_confirmation_required_ == confirmationRequired)
        return;
    monitoring_tracks_ = std::move(tracks);
    monitoring_confirmation_required_ = confirmationRequired;
    if (!confirmationRequired && clip_open_)
        setPlaceholderText({});
    emit monitoringChanged();
}

void EditPlayerAdapter::beginScrub() {
    if (!scrubbing_) {
        scrubbing_ = true;
        dispatchTransport(false);
        emit scrubbingChanged();
    }
}

void EditPlayerAdapter::endScrub() {
    // After the resume, not before: a surface that hides itself while scrubbing
    // and while playing would otherwise flash for the one frame between the two.
    if (scrubbing_) {
        scrubbing_ = false;
        dispatchTransport(pending_seek_.has_value() || seek_dispatch_active_);
        emit scrubbingChanged();
    }
}

void EditPlayerAdapter::openClip(const QString& master_path, qint64 duration_ms) {
    pending_timeline_current_.reset();
    duration_ms_ = duration_ms;
    if (playing_) {
        playing_ = false;
        emit playingChanged();
    }
    sink_->clear();
    setPlaceholderText(QCoreApplication::translate("EditPlayerAdapter", "Opening clip\xe2\x80\xa6"));
    const auto refresh = currentScreenRefreshHz();
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, cancelled = opening_cancelled_, master_path, duration_ms, refresh] {
            worker->setOpeningCancellation(cancelled);
            worker->open(master_path, duration_ms, refresh);
        },
        Qt::QueuedConnection);
}

void EditPlayerAdapter::closeClip() {
    ++measurement_session_;
    measurement_sample_.reset();
    opening_cancelled_->store(true);
    opening_cancelled_ = std::make_shared<std::atomic<bool>>(false);
    ++monitoring_request_;
    pending_timeline_current_.reset();
    duration_ms_ = 0;
    timeline_revision_.reset();
    published_snapshot_.reset();
    unavailable_sources_.clear();
    timeline_monitoring_revision_ = 0;
    bounds_ = {};
    emit boundsChanged();
    pending_seek_.reset();
    request_generation_ = sink_->invalidate();
    presented_time_us_.reset();
    requested_time_us_ = 0;
    seek_pending_ = false;
    desired_playing_ = false;
    transport_pending_ = false;
    ++transport_command_;
    current_transport_->store(transport_command_);
    emit transportChanged();
    setPreviewState(Empty);
    emit previewChanged();
    setMonitoringState({}, false);
    if (playing_) {
        playing_ = false;
        emit playingChanged();
    }
    if (clip_open_) {
        clip_open_ = false;
        emit clipOpenChanged();
    }
    sink_->clear();
    setPlaceholderText(QCoreApplication::translate("EditPlayerAdapter", "Preview unavailable"));
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation = request_generation_] {
            worker->setRequestGeneration(generation);
            worker->close();
        },
        Qt::QueuedConnection);
}

void EditPlayerAdapter::seekUs(qint64 position_us) {
    if (session_)
        session_->cancelRangeGesture();
    bounds_.seek(position_us);
    emit boundsChanged();
    const auto generation = sink_->invalidate();
    request_generation_ = generation;
    requested_time_us_ = position_us;
    seek_pending_ = session_ ? session_->workspace().duration() > 0 : duration_ms_ > 0;
    setPreviewState(seek_pending_ ? SeekPending : Empty);
    emit previewChanged();
    transport_pending_ = true;
    pending_seek_ = SeekRequest{position_us, generation, ++transport_command_, desired_playing_ && !scrubbing_};
    current_transport_->store(transport_command_);
    emit transportChanged();
    dispatchSeek();
}

void EditPlayerAdapter::dispatchSeek() {
    if (seek_dispatch_active_ || !pending_seek_)
        return;
    seek_dispatch_active_ = true;
    // A seek must observe the workspace queued before it, even if a later edit arrives.
    pending_timeline_current_.reset();
    const auto request = *pending_seek_;
    pending_seek_.reset();
    QPointer<EditPlayerAdapter> self(this);
    const auto submitted = std::chrono::steady_clock::now();
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, current = current_transport_, request, self, submitted, region = bounds_.interval(),
         repeat = bounds_.repeating()]() {
            worker->recordQueueWait(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - submitted)
                    .count(),
                false);
            if (current->load() == request.command) {
                worker->setPlaybackBounds(region, repeat);
                worker->requestTransport(request.command, request.generation, request.time_us, true, request.playing);
            }
            if (self)
                QMetaObject::invokeMethod(
                    self,
                    [self]() {
                        if (!self)
                            return;
                        self->seek_dispatch_active_ = false;
                        self->dispatchSeek();
                    },
                    Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void EditPlayerAdapter::setVolume(double volume) {
    volume = std::clamp(volume, 0.0, 1.0);
    if (volume_ == volume)
        return;
    volume_ = volume;
    QMetaObject::invokeMethod(worker_, "setVolume", Qt::QueuedConnection, Q_ARG(double, volume));
    emit volumeChanged();
}

// The screen the window actually sits on, not the primary one -- dragging
// ExoSnap from a 60 Hz to a 144 Hz panel should change the cadence.
double EditPlayerAdapter::currentScreenRefreshHz() const {
    if (!item_.isNull() && item_->window() != nullptr && item_->window()->screen() != nullptr)
        return item_->window()->screen()->refreshRate();
    const QScreen* primary = QGuiApplication::primaryScreen();
    return primary != nullptr ? primary->refreshRate() : 0.0;
}

} // namespace exosnap::quick
