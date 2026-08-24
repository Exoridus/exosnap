#pragma once

#include "EditPlayerAdapter.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <exosnap/engine/edit_player_session.h>

#include <memory>

namespace exosnap::quick {

// The decoder session's single owning thread.
//
// EditPlayerSession documents Open/Close/Play/Pause/SeekTo as calls from ONE
// thread. The Widgets surface satisfied that by making the GUI thread the owner,
// which is why opening a clip froze it. Everything here runs on
// EditPlayerAdapter's worker thread instead, so the contract still holds and the
// GUI thread never waits on a container read.
//
// Decoded frames deliberately do NOT pass through this object: the session's own
// decode/seek thread hands them straight to EditPlayerFrameSink.
class EditPlayerWorker : public QObject {
    Q_OBJECT
  public:
    explicit EditPlayerWorker(std::shared_ptr<EditPlayerFrameSink> sink);
    ~EditPlayerWorker() override;

  public slots:
    void open(const QString& master_path, qint64 duration_ms, double screen_hz);
    void close();
    void play(qint64 from_ms);
    void pause();
    void seek(qint64 position_ms);
    void setScreenRefreshHz(double screen_hz);

  signals:
    void openFinished(bool opened, const QString& error);
    void positionAdvanced(qint64 position_ms);
    void reachedEnd();

  private:
    void ensureTimer();
    // Re-publishes the session's OWN clock into the item's present gate. Must run
    // after every Play/Pause/SeekTo, not only on the tick: the session resets its
    // clock to -1 on pause (and SeekTo pauses first), and without propagating
    // that reset every backward scrub and every trim-handle preview is dropped by
    // the gate and the picture stays frozen.
    void syncClock();
    void onTick();

    std::shared_ptr<EditPlayerFrameSink> sink_;
    std::unique_ptr<exosnap::engine::EditPlayerSession> session_;
    QTimer* tick_timer_ = nullptr;
    QElapsedTimer elapsed_;
    qint64 duration_ms_ = 0;
    qint64 position_ms_ = 0;
    double screen_hz_ = 0.0;
};

} // namespace exosnap::quick
