#include "EditPlayerWorker.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>

namespace exosnap::quick {
namespace {

// Fallback tick, used only until a clip is open (or when its container declares
// no usable frame rate).
constexpr int kWorkerFallbackTickMs = 16;

} // namespace

EditPlayerWorker::EditPlayerWorker(std::shared_ptr<EditPlayerFrameSink> sink) : sink_(std::move(sink)) {
}

EditPlayerWorker::~EditPlayerWorker() {
    close();
}

void EditPlayerWorker::open(const QString& master_path, qint64 duration_ms, double screen_hz) {
    close();
    duration_ms_ = duration_ms;
    screen_hz_ = screen_hz;
    session_ = std::make_unique<exosnap::engine::EditPlayerSession>();
    std::string error;
    if (!session_->Open(std::filesystem::path(master_path.toStdWString()), error)) {
        session_.reset();
        emit openFinished(false, QString::fromStdString(error));
        return;
    }
    // Delivered straight from the decode/seek thread into the scene-graph item,
    // with no UI-thread hop: the item's mailbox is thread-safe by design, and a
    // per-frame hop would undo the point of the GPU path.
    auto sink = sink_;
    session_->SetOnFrameReady([sink](exosnap::engine::RawDecodedVideoFrame frame) { sink->deliver(std::move(frame)); });
    ensureTimer();
    tick_timer_->setInterval(EditPreviewTickMsFor(session_->VideoFrameRate(), screen_hz_));
    // Poster frame: show the clip's first frame instead of the placeholder while
    // the user is still reviewing. Publishing the clock matters here too -- a
    // second clip opened over a first would otherwise leave the gate holding the
    // previous clip's value and drop this very frame.
    session_->SeekTo(0);
    syncClock();
    emit openFinished(true, {});
}

void EditPlayerWorker::close() {
    if (tick_timer_ != nullptr)
        tick_timer_->stop();
    if (session_) {
        session_->SetOnFrameReady({});
        session_->Close();
        session_.reset();
    }
    position_ms_ = 0;
    sink_->publishClock(-1);
}

void EditPlayerWorker::play(qint64 from_ms) {
    if (!session_)
        return;
    position_ms_ = from_ms;
    ensureTimer();
    tick_timer_->setInterval(EditPreviewTickMsFor(session_->VideoFrameRate(), screen_hz_));
    elapsed_.restart();
    tick_timer_->start();
    // Continuous decode is only engaged when there is audio to pace it against;
    // a silent clip is driven entirely by the per-tick SeekTo fallback below,
    // since continuous decode would race through the file unthrottled for frames
    // nothing consumes.
    if (session_->HasAudioStream())
        session_->Play(position_ms_ * 1000);
    syncClock();
}

void EditPlayerWorker::pause() {
    if (tick_timer_ != nullptr)
        tick_timer_->stop();
    if (!session_)
        return;
    session_->Pause();
    syncClock();
}

void EditPlayerWorker::seek(qint64 position_ms) {
    position_ms_ = position_ms;
    if (!session_)
        return;
    session_->SeekTo(position_ms * 1000);
    // A seek result is the frame the user asked for: never gate it.
    syncClock();
}

void EditPlayerWorker::setScreenRefreshHz(double screen_hz) {
    screen_hz_ = screen_hz;
    if (tick_timer_ != nullptr && session_)
        tick_timer_->setInterval(EditPreviewTickMsFor(session_->VideoFrameRate(), screen_hz_));
}

void EditPlayerWorker::ensureTimer() {
    if (tick_timer_ != nullptr)
        return;
    tick_timer_ = new QTimer(this);
    // Coarse timers are allowed a 5% slop, which at 144 fps (6 ms) is most of a
    // frame.
    tick_timer_->setTimerType(Qt::PreciseTimer);
    tick_timer_->setInterval(kWorkerFallbackTickMs);
    connect(tick_timer_, &QTimer::timeout, this, &EditPlayerWorker::onTick);
}

void EditPlayerWorker::syncClock() {
    sink_->publishClock(session_ ? session_->ClockSnapshotUs() : -1);
}

void EditPlayerWorker::onTick() {
    if (!session_)
        return;
    const qint64 total = std::max<qint64>(duration_ms_, 0);
    if (session_->HasAudioStream()) {
        // Audio is the pacing AND position source of truth while it exists.
        position_ms_ = std::clamp<qint64>(session_->CurrentPositionMs(), 0, total);
        sink_->publishClock(session_->ClockSnapshotUs());
    } else {
        position_ms_ = std::clamp<qint64>(position_ms_ + elapsed_.restart(), 0, total);
        session_->SeekTo(position_ms_ * 1000);
        syncClock();
    }
    emit positionAdvanced(position_ms_);
    if (total > 0 && position_ms_ >= total) {
        tick_timer_->stop();
        session_->Pause();
        syncClock();
        emit reachedEnd();
    }
}

} // namespace exosnap::quick
