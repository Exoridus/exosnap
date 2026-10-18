#pragma once

#include "EditPlayerAdapter.h"
#include "models/EditWorkspace.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <exosnap/engine/edit_player_session.h>
#include <exosnap/engine/edit_timeline_render.h>
#include <exosnap/engine/wasapi_audio_render.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

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
    friend class EditPlayerWorkerTestPeer;

  public:
    explicit EditPlayerWorker(std::shared_ptr<EditPlayerFrameSink> sink);
    ~EditPlayerWorker() override;
    void setTimeline(edit::Workspace workspace);
    // Cumulative worker/session costs. Read on the owning worker thread.
    [[nodiscard]] QVariantMap performanceSnapshot() const;
    void recordQueueWait(qint64 elapsed_ns, bool timeline);
    void setRequestGeneration(uint64_t generation) {
        request_generation_ = generation;
    }

  public slots:
    void open(const QString& master_path, qint64 duration_ms, double screen_hz);
    void close();
    void play(qint64 from_ms);
    void playUs(qint64 from_us);
    void pause();
    void seek(qint64 position_ms);
    void seekUs(qint64 target_us);
    void setScreenRefreshHz(double screen_hz);
    void setVolume(double volume);
    void confirmMonitoring(const QStringList& track_ids, bool overlap_confirmed);

  signals:
    void openFinished(bool opened, const QString& error);
    void openFinishedForGeneration(qulonglong generation, bool opened, const QString& error);
    void playbackStateChanged(bool playing);
    void positionAdvancedUs(qint64 position_us);
    void positionAdvanced(qint64 position_ms);
    void reachedEnd();
    void monitoringChanged(QVariantList tracks, bool confirmation_required);
    void monitoringRejected(QString error);
    void monitorSelectionConfirmed(qulonglong asset, exosnap::engine::MonitorSelectionSnapshot selection);

  private:
    void ensureTimer();
    void publishOpenResult(bool opened, const QString& error);
    void publishPlaybackState(bool playing);
    // Re-publishes the session's OWN clock into the item's present gate. Must run
    // after every Play/Pause/SeekTo, not only on the tick: the session resets its
    // clock to -1 on pause (and SeekTo pauses first), and without propagating
    // that reset every backward scrub and every trim-handle preview is dropped by
    // the gate and the picture stays frozen.
    void syncClock();
    void onTick();
    void seekTimeline(qint64 position_us, bool resume, bool preserve_audio = false);
    void closeSource();
    void openSource(const QString& master_path, qint64 duration_ms, double screen_hz);
    engine::EditTimelineReader* timelineAudioReader();
    bool startTimelineAudio(qint64 position_us);
    bool fillTimelineAudio();
    qint64 timelinePositionUs() const;
    bool renderTimeline(qint64 position_us, engine::VideoReadMode mode = engine::VideoReadMode::Sequential);
    void presentBlank(qint64 position_us);
    void publishMonitoring();
    void tickRenderTimeline();
    bool canReuseSource(const edit::Asset& asset) const;
    bool bindTimelineSource(const edit::Clip& clip, const edit::Asset& asset);

    std::shared_ptr<EditPlayerFrameSink> sink_;
    std::unique_ptr<exosnap::engine::EditPlayerSession> session_;
    QTimer* tick_timer_ = nullptr;
    QElapsedTimer elapsed_;
    qint64 duration_ms_ = 0;
    qint64 position_ms_ = 0;
    qint64 position_us_ = 0;
    double screen_hz_ = 0.0;
    edit::Workspace workspace_;
    edit::Id active_clip_ = 0;
    bool timeline_mode_ = false;
    bool timeline_playing_ = false;
    bool published_playing_ = false;
    double volume_ = 1.0;
    uint64_t request_generation_ = 0;
    bool render_mode_ = false;
    std::unique_ptr<engine::EditTimelineReader> render_reader_;
    std::unique_ptr<engine::EditTimelineReader> timeline_audio_reader_;
    std::unique_ptr<engine::WasapiAudioRenderer> render_audio_;
    engine::EditRenderPlan render_plan_;
    engine::TimelineRenderSnapshot render_snapshot_;
    qint64 render_origin_us_ = 0;
    int64_t audio_sample_ = 0;
    uint64_t monitor_revision_ = 0;
    quint64 timeline_updates_ = 0;
    quint64 session_opens_ = 0;
    quint64 session_closes_ = 0;
    qint64 session_open_ns_ = 0;
    qint64 session_close_ns_ = 0;
    quint64 seek_dispatches_ = 0;
    qint64 seek_dispatch_ns_ = 0;
    qint64 timeline_queue_wait_ns_ = 0;
    qint64 seek_queue_wait_ns_ = 0;
    engine::EditPlayerSession::SeekPerformance retired_seek_performance_;
    std::optional<edit::Asset> active_source_;
    std::optional<edit::StreamRef> active_stream_;
    std::filesystem::file_time_type active_source_write_time_;
    std::uintmax_t active_source_size_ = 0;
    qint64 active_timeline_offset_ = 0;
};

} // namespace exosnap::quick

Q_DECLARE_METATYPE(exosnap::engine::MonitorSelectionSnapshot)
