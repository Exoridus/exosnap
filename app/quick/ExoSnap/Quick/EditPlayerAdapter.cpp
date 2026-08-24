#include "EditPlayerAdapter.h"

#include "EditPlayerWorker.h"
#include "EditSessionAdapter.h"
#include "ExoEditPlayerItem.h"

#include <QGuiApplication>
#include <QQuickWindow>
#include <QScreen>

#include <algorithm>
#include <cmath>
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

void EditPlayerFrameSink::publishClock(int64_t media_time_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (item_ != nullptr)
        item_->setClockUs(media_time_us);
}

void EditPlayerFrameSink::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (item_ != nullptr)
        item_->clearFrame();
}

EditPlayerAdapter::EditPlayerAdapter(QObject* parent) : QObject(parent) {
    worker_ = new EditPlayerWorker(sink_);
    worker_->moveToThread(&worker_thread_);
    connect(&worker_thread_, &QThread::finished, worker_, &QObject::deleteLater);

    connect(worker_, &EditPlayerWorker::openFinished, this, [this](bool opened, const QString& error) {
        if (clip_open_ != opened) {
            clip_open_ = opened;
            emit clipOpenChanged();
        }
        setPlaceholderText(opened ? QString() : (error.isEmpty() ? QStringLiteral("Preview unavailable") : error));
    });
    connect(worker_, &EditPlayerWorker::positionAdvanced, this, [this](qint64 position_ms) {
        if (session_ != nullptr)
            session_->setPositionMs(position_ms);
    });
    connect(worker_, &EditPlayerWorker::reachedEnd, this, [this]() {
        if (!playing_)
            return;
        playing_ = false;
        emit playingChanged();
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
    session_ = session;
    if (session_ == nullptr)
        return;
    connect(session_, &EditSessionAdapter::clipOpened, this, &EditPlayerAdapter::openClip);
    connect(session_, &EditSessionAdapter::clipClosed, this, &EditPlayerAdapter::closeClip);
    connect(session_, &EditSessionAdapter::seekRequested, this, &EditPlayerAdapter::seek);
}

void EditPlayerAdapter::attachPlayerItem(ExoEditPlayerItem* item) {
    item_ = item;
    sink_->attach(item);
}

void EditPlayerAdapter::detachPlayerItem(ExoEditPlayerItem* item) {
    sink_->detach(item);
    if (item_ == item)
        item_.clear();
}

bool EditPlayerAdapter::playing() const noexcept {
    return playing_;
}

bool EditPlayerAdapter::clipOpen() const noexcept {
    return clip_open_;
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
    if (playing == playing_)
        return;
    if (playing && (duration_ms_ <= 0 || !clip_open_))
        return; // unknown duration or no clip: nothing to play against
    playing_ = playing;
    if (playing_) {
        const qint64 from = session_ != nullptr ? session_->positionMs() : 0;
        QMetaObject::invokeMethod(worker_, "setScreenRefreshHz", Qt::QueuedConnection,
                                  Q_ARG(double, currentScreenRefreshHz()));
        QMetaObject::invokeMethod(worker_, "play", Qt::QueuedConnection, Q_ARG(qint64, from));
    } else {
        QMetaObject::invokeMethod(worker_, "pause", Qt::QueuedConnection);
    }
    emit playingChanged();
}

void EditPlayerAdapter::beginScrub() {
    resume_after_scrub_ = playing_;
    setPlaying(false);
}

void EditPlayerAdapter::endScrub() {
    if (resume_after_scrub_)
        setPlaying(true);
    resume_after_scrub_ = false;
}

void EditPlayerAdapter::openClip(const QString& master_path, qint64 duration_ms) {
    duration_ms_ = duration_ms;
    if (playing_) {
        playing_ = false;
        emit playingChanged();
    }
    sink_->clear();
    setPlaceholderText(QStringLiteral("Opening clip\xe2\x80\xa6"));
    QMetaObject::invokeMethod(worker_, "open", Qt::QueuedConnection, Q_ARG(QString, master_path),
                              Q_ARG(qint64, duration_ms), Q_ARG(double, currentScreenRefreshHz()));
}

void EditPlayerAdapter::closeClip() {
    duration_ms_ = 0;
    if (playing_) {
        playing_ = false;
        emit playingChanged();
    }
    if (clip_open_) {
        clip_open_ = false;
        emit clipOpenChanged();
    }
    sink_->clear();
    setPlaceholderText(QStringLiteral("Preview unavailable"));
    QMetaObject::invokeMethod(worker_, "close", Qt::QueuedConnection);
}

void EditPlayerAdapter::seek(qint64 position_ms) {
    QMetaObject::invokeMethod(worker_, "seek", Qt::QueuedConnection, Q_ARG(qint64, position_ms));
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
