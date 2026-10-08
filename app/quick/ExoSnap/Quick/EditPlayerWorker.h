#pragma once

#include "EditPlayerAdapter.h"
#include "models/EditWorkspace.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <exosnap/engine/edit_player_session.h>
#include <exosnap/engine/edit_timeline_render.h>
#include <exosnap/engine/wasapi_audio_render.h>

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
    void setTimeline(edit::Workspace workspace);
    void setRequestGeneration(uint64_t generation) {
        request_generation_ = generation;
    }

  public slots:
    void open(const QString& master_path, qint64 duration_ms, double screen_hz);
    void close();
    void play(qint64 from_ms);
    void pause();
    void seek(qint64 position_ms);
    void setScreenRefreshHz(double screen_hz);
    void setVolume(double volume);

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
    void seekTimeline(qint64 position_ms, bool resume);
    bool renderTimeline(qint64 position_ms);
    void tickRenderTimeline();

    std::shared_ptr<EditPlayerFrameSink> sink_;
    std::unique_ptr<exosnap::engine::EditPlayerSession> session_;
    QTimer* tick_timer_ = nullptr;
    QElapsedTimer elapsed_;
    qint64 duration_ms_ = 0;
    qint64 position_ms_ = 0;
    double screen_hz_ = 0.0;
    edit::Workspace workspace_;
    edit::Id active_clip_ = 0;
    bool timeline_mode_ = false;
    bool timeline_playing_ = false;
    double volume_ = 1.0;
    uint64_t request_generation_ = 0;
    bool render_mode_ = false;
    std::unique_ptr<engine::EditTimelineReader> render_reader_;
    std::unique_ptr<engine::WasapiAudioRenderer> render_audio_;
    engine::EditRenderPlan render_plan_;
    qint64 render_origin_ms_ = 0;
    int64_t audio_sample_ = 0;
};

} // namespace exosnap::quick
