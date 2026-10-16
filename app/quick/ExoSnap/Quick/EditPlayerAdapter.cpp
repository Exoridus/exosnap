#include "EditPlayerAdapter.h"
#include "EditRenderText.h"
#include <QCoreApplication>

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
    item_ = item;
}

void EditPlayerFrameSink::detach(const ExoEditPlayerItem* item) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (item_ == item)
        item_ = nullptr;
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
    item_->presentFrame(std::move(frame), timeline_time_us);
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
    return ++generation_;
}

void EditPlayerFrameSink::deliverTimeline(exosnap::engine::TimelineVideoFrame frame, uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (item_ && generation == generation_)
        item_->presentTimelineFrame(std::move(frame));
}

void EditPlayerFrameSink::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (item_ != nullptr)
        item_->clearFrame();
}

EditPlayerAdapter::EditPlayerAdapter(QObject* parent) : QObject(parent) {
    qRegisterMetaType<engine::MonitorSelectionSnapshot>();
    worker_ = new EditPlayerWorker(sink_);
    worker_->moveToThread(&worker_thread_);
    connect(&worker_thread_, &QThread::finished, worker_, &QObject::deleteLater);

    connect(worker_, &EditPlayerWorker::openFinishedForGeneration, this,
            [this](qulonglong generation, bool opened, const QString& error) {
                if (generation != request_generation_)
                    return;
                if (clip_open_ != opened) {
                    clip_open_ = opened;
                    emit clipOpenChanged();
                }
                // An open timeline can still report an unavailable segment; that is a
                // degraded preview, not a ready one.
                const bool failed = !opened || !error.isEmpty();
                setPlaceholderText(
                    !failed ? QString()
                            : (error.isEmpty() ? QCoreApplication::translate("EditPlayerAdapter", "Preview unavailable")
                                               : TranslateEditRenderReason(error)));
                if (failed) {
                    seek_pending_ = false;
                    setPreviewState(duration_ms_ <= 0 ? Empty : presented_time_us_ ? Degraded : Failed);
                    emit previewChanged();
                }
            });
    connect(worker_, &EditPlayerWorker::positionAdvancedUs, this, [this](qint64 position_us) {
        if (session_ != nullptr)
            session_->setPositionUs(position_us);
        if (!seek_pending_ && requested_time_us_ != position_us) {
            requested_time_us_ = position_us;
            emit previewChanged();
        }
    });
    connect(worker_, &EditPlayerWorker::playbackStateChanged, this, [this](bool playing) {
        if (playing == desired_playing_)
            transport_pending_ = false;
        if (playing_ == playing)
            return;
        playing_ = playing;
        emit playingChanged();
    });
    connect(worker_, &EditPlayerWorker::reachedEnd, this, [this]() {
        desired_playing_ = false;
        transport_pending_ = false;
        if (!playing_)
            return;
        playing_ = false;
        emit playingChanged();
    });

    connect(worker_, &EditPlayerWorker::monitoringChanged, this, &EditPlayerAdapter::setMonitoringState);
    connect(worker_, &EditPlayerWorker::monitoringRejected, this, [this](const QString& error) {
        setPlaying(false);
        setPlaceholderText(error);
    });
    connect(worker_, &EditPlayerWorker::monitorSelectionConfirmed, this,
            [this](qulonglong asset, engine::MonitorSelectionSnapshot selection) {
                if (session_)
                    session_->setAssetMonitorSelection(asset, std::move(selection));
            });

    worker_thread_.setObjectName(QStringLiteral("exosnap-edit-player"));
    worker_thread_.start();
}

EditPlayerAdapter::~EditPlayerAdapter() {
    // Stop delivery first: the decode thread must not reach a half-destroyed
    // item, and close() is what joins that thread.
    sink_->detach(item_.data());
    QMetaObject::invokeMethod(worker_, "close", Qt::BlockingQueuedConnection);
    worker_thread_.quit();
    worker_thread_.wait();
}

void EditPlayerAdapter::setSession(EditSessionAdapter* session) {
    pending_timeline_current_.reset();
    if (session_)
        disconnect(session_, nullptr, this, nullptr);
    session_ = session;
    if (session_ == nullptr)
        return;
    connect(session_, &EditSessionAdapter::workspaceChanged, this, [this]() {
        duration_ms_ = session_->durationMs();
        const auto& current = session_->workspace();
        if (timeline_revision_ == current.revision() && timeline_monitoring_revision_ == current.monitoringRevision())
            return;
        QElapsedTimer copy_clock;
        copy_clock.start();
        auto workspace = session_->workspace();
        workspace_copy_ns_ += copy_clock.nsecsElapsed();
        timeline_revision_ = workspace.revision();
        timeline_monitoring_revision_ = workspace.monitoringRevision();
        const auto generation = sink_->invalidate();
        request_generation_ = generation;
        pending_seek_.reset();
        requested_time_us_ = workspace.playhead();
        seek_pending_ = workspace.duration() > 0;
        setPreviewState(!seek_pending_ ? Empty : presented_time_us_ ? SeekPending : Opening);
        emit previewChanged();
        if (pending_timeline_current_)
            pending_timeline_current_->store(false);
        const auto current_update = std::make_shared<std::atomic<bool>>(true);
        pending_timeline_current_ = current_update;
        const auto submitted = std::chrono::steady_clock::now();
        QMetaObject::invokeMethod(
            worker_,
            [worker = worker_, workspace = std::move(workspace), generation, submitted, current_update]() mutable {
                if (!current_update->load())
                    return;
                worker->recordQueueWait(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - submitted)
                        .count(),
                    true);
                worker->setRequestGeneration(generation);
                worker->setTimeline(std::move(workspace));
            },
            Qt::QueuedConnection);
    });
    connect(session_, &EditSessionAdapter::clipClosed, this, &EditPlayerAdapter::closeClip);
    connect(session_, &EditSessionAdapter::seekRequestedUs, this, &EditPlayerAdapter::seekUs);
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
        setPreviewState(Ready);
        emit previewChanged();
    });
    connect(item, &ExoEditPlayerItem::hasFrameChanged, this, [this]() {
        if (!item_ || item_->hasFrame())
            return;
        presented_time_us_.reset();
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
    if (clip_open_ == clip_open)
        return;
    clip_open_ = clip_open;
    emit clipOpenChanged();
}

void EditPlayerAdapter::setPlaceholderText(const QString& text) {
    if (placeholder_text_ == text)
        return;
    placeholder_text_ = text;
    emit placeholderTextChanged();
}

void EditPlayerAdapter::togglePlay() {
    setPlaying(!playing_);
}

void EditPlayerAdapter::setPlaying(bool playing) {
    if ((!transport_pending_ && playing == playing_) || (transport_pending_ && playing == desired_playing_))
        return;
    if (playing && (duration_ms_ <= 0 || !clip_open_ || monitoring_confirmation_required_))
        return;
    desired_playing_ = playing;
    transport_pending_ = true;
    pending_timeline_current_.reset();
    if (playing) {
        const qint64 from = session_ != nullptr ? session_->positionUs() : 0;
        QMetaObject::invokeMethod(worker_, "setScreenRefreshHz", Qt::QueuedConnection,
                                  Q_ARG(double, currentScreenRefreshHz()));
        QMetaObject::invokeMethod(worker_, "playUs", Qt::QueuedConnection, Q_ARG(qint64, from));
    } else {
        QMetaObject::invokeMethod(worker_, "pause", Qt::QueuedConnection);
    }
}

void EditPlayerAdapter::confirmMonitoring(QStringList trackIds, bool overlapConfirmed) {
    setPlaying(false);
    pending_timeline_current_.reset();
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, trackIds = std::move(trackIds), overlapConfirmed]() mutable {
            worker->confirmMonitoring(std::move(trackIds), overlapConfirmed);
        },
        Qt::QueuedConnection);
}

void EditPlayerAdapter::setMonitoringState(QVariantList tracks, bool confirmationRequired) {
    if (confirmationRequired)
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
    resume_after_scrub_ = playing_;
    setPlaying(false);
    if (!scrubbing_) {
        scrubbing_ = true;
        emit scrubbingChanged();
    }
}

void EditPlayerAdapter::endScrub() {
    if (resume_after_scrub_)
        setPlaying(true);
    resume_after_scrub_ = false;
    // After the resume, not before: a surface that hides itself while scrubbing
    // and while playing would otherwise flash for the one frame between the two.
    if (scrubbing_) {
        scrubbing_ = false;
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
    QMetaObject::invokeMethod(worker_, "open", Qt::QueuedConnection, Q_ARG(QString, master_path),
                              Q_ARG(qint64, duration_ms), Q_ARG(double, currentScreenRefreshHz()));
}

void EditPlayerAdapter::closeClip() {
    pending_timeline_current_.reset();
    duration_ms_ = 0;
    timeline_revision_.reset();
    timeline_monitoring_revision_ = 0;
    pending_seek_.reset();
    request_generation_ = sink_->invalidate();
    presented_time_us_.reset();
    requested_time_us_ = 0;
    seek_pending_ = false;
    desired_playing_ = false;
    transport_pending_ = false;
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
    QMetaObject::invokeMethod(worker_, "close", Qt::QueuedConnection);
}

void EditPlayerAdapter::seekUs(qint64 position_us) {
    setPlaying(false);
    const auto generation = sink_->invalidate();
    request_generation_ = generation;
    requested_time_us_ = position_us;
    seek_pending_ = session_ ? session_->workspace().duration() > 0 : duration_ms_ > 0;
    setPreviewState(seek_pending_ ? SeekPending : Empty);
    emit previewChanged();
    pending_seek_ = SeekRequest{position_us, generation};
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
        [worker = worker_, request, self, submitted]() {
            worker->recordQueueWait(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - submitted)
                    .count(),
                false);
            worker->setRequestGeneration(request.generation);
            worker->seekUs(request.time_us);
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
