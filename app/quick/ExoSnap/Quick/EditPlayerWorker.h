#pragma once

#include "EditPlaybackSnapshot.h"
#include "EditPlayerAdapter.h"
#include "models/EditTransportBounds.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <exosnap/engine/edit_player_session.h>
#include <exosnap/engine/edit_timeline_audio.h>
#include <exosnap/engine/edit_timeline_render.h>
#include <exosnap/engine/wasapi_audio_render.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace exosnap::quick {

// Owns playback commands on the adapter's worker thread.
// Raw/composed video and source PCM have separate exclusive execution contexts.
// Video completions return here for generation-checked sink adoption.
class EditPlayerWorker : public QObject {
    Q_OBJECT
    friend class EditPlayerWorkerTestPeer;

  public:
    explicit EditPlayerWorker(std::shared_ptr<EditPlayerFrameSink> sink);
    ~EditPlayerWorker() override;
    void setTimeline(const edit::Workspace& workspace);
    void setTimeline(std::shared_ptr<const EditPlaybackSnapshot> snapshot, qint64 requested_position_us,
                     const std::function<bool()>& may_play = {});
    // Cumulative worker/session costs. Read on the owning worker thread.
    [[nodiscard]] QVariantMap performanceSnapshot() const;
    void recordQueueWait(qint64 elapsed_ns, bool timeline);
    void setRequestGeneration(uint64_t generation) {
        request_generation_ = generation;
    }
    void requestTransport(uint64_t command, uint64_t generation, qint64 target_us, bool seek, bool playing);
    void acknowledgeTransport();
    void setOpeningCancellation(std::shared_ptr<std::atomic<bool>> cancelled) {
        opening_cancelled_ = std::move(cancelled);
    }
    void setTransportCommand(uint64_t command) {
        transport_command_ = command;
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
    void setPlaybackBounds(edit::PlaybackInterval interval, bool repeat);
    void setSoloTracks(std::vector<uint64_t> tracks);
    void confirmMonitoring(const QStringList& track_ids, bool overlap_confirmed, qulonglong request = 0);

  signals:
    void openFinished(bool opened, const QString& error);
    void openFinishedForGeneration(qulonglong generation, bool opened, const QString& error);
    void openFinishedForCommand(qulonglong command, qulonglong generation, bool opened, const QString& error);
    void playbackStateChanged(bool playing);
    void transportStateChanged(qulonglong command, qulonglong generation, bool playing);
    void transportCompleted(qulonglong command, qulonglong generation, bool playing);
    void transportEnded(qulonglong command, qulonglong generation);
    void positionAdvancedUs(qint64 position_us);
    void positionAdvancedForCommand(qulonglong command, qulonglong generation, qint64 position_us);
    void positionAdvanced(qint64 position_ms);
    void reachedEnd();
    void monitoringChanged(QVariantList tracks, bool confirmation_required);
    void monitoringRejected(QString error);
    void monitoringRejectedForCommand(qulonglong command, qulonglong generation, QString error);
    void monitoringStateChangedForGeneration(qulonglong generation, QVariantList tracks, bool confirmation_required);
    void monitorSelectionConfirmed(qulonglong request, qulonglong asset,
                                   exosnap::engine::MonitorSelectionSnapshot selection);

  private:
    void ensureTimer();
    void ensureVideoPump();
    void publishOpenResult(bool opened, const QString& error);
    void publishPlaybackState(bool playing);
    void publishPosition();
    // Timeline reads already select against the output clock. A second gate on
    // source-local PTS would reject valid remapped frames after a seek.
    void syncClock();
    void onTick();
    void seekTimeline(qint64 position_us, bool resume, bool preserve_audio = false);
    void seekWithIntent(qint64 target_us, bool resume);
    engine::TimelineRenderSnapshot makeRenderSnapshot(const EditPlaybackSnapshot& workspace);
    void applyTimeline(std::shared_ptr<const EditPlaybackSnapshot> workspace, qint64 position_us,
                       const std::function<bool()>& may_play);
    void prepareRevision(std::shared_ptr<const EditPlaybackSnapshot> workspace, const std::function<bool()>& may_play);
    bool adoptPreparedRevision();
    void cancelPreparedRevision();
    void updateAudibleControls(const EditPlaybackSnapshot& requested);
    [[nodiscard]] uint64_t timelineOutputFrames() const;
    bool startTimelineAudio(qint64 position_us);
    bool fillTimelineAudio();
    bool publishAudioControls();
    qint64 timelinePositionUs() const;
    bool renderTimeline(qint64 position_us, engine::VideoReadMode mode = engine::VideoReadMode::Sequential);
    void presentBlank(qint64 position_us);
    void publishMonitoring();
    void publishMonitoringState(QVariantList tracks, bool confirmation_required);
    void rejectMonitoring(const QString& error);
    void tickRenderTimeline();

    std::shared_ptr<EditPlayerFrameSink> sink_;
    QTimer* tick_timer_ = nullptr;
    QElapsedTimer elapsed_;
    qint64 duration_ms_ = 0;
    qint64 position_ms_ = 0;
    qint64 position_us_ = 0;
    double screen_hz_ = 0.0;
    std::shared_ptr<const EditPlaybackSnapshot> workspace_ = std::make_shared<EditPlaybackSnapshot>(edit::Workspace{});
    qint64 snapshot_position_us_ = 0;
    edit::Id active_clip_ = 0;
    bool timeline_mode_ = false;
    bool timeline_playing_ = false;
    bool published_playing_ = false;
    bool source_opened_ = false;
    bool standalone_probe_pending_ = false;
    bool standalone_play_requested_ = false;
    qint64 standalone_position_us_ = 0;
    QString source_open_error_;
    double volume_ = 1.0;
    uint64_t request_generation_ = 0;
    uint64_t transport_command_ = 0;
    bool render_mode_ = false;
    struct VideoPump;
    std::unique_ptr<VideoPump> video_pump_;
    std::shared_ptr<const engine::TimelineRenderSnapshot> video_snapshot_;
    uint64_t video_epoch_ = 0;
    std::function<bool(const std::atomic<bool>&)> video_before_read_;
    std::function<bool(const std::atomic<bool>&)> video_before_open_;
    std::function<bool(const std::atomic<bool>&)> audio_before_read_;
    std::shared_ptr<engine::TimelineAudioProducer> audio_producer_;
    struct PendingRevision;
    std::unique_ptr<PendingRevision> pending_revision_;
    std::shared_ptr<engine::TimelineAudioProducer> standby_audio_producer_;
    std::optional<engine::TimelineSnapshot> audible_controls_timeline_;
    uint64_t audible_control_revision_ = 0;
    uint64_t next_audio_stream_generation_ = 0;
    bool adopting_audio_revision_ = false;
    uint64_t revision_handoffs_ = 0;
    uint64_t late_video_mapping_ticks_ = 0;
    uint64_t revision_lead_before_reset_frames_ = 0;
    std::unique_ptr<engine::WasapiAudioRenderer> render_audio_;
    engine::EditRenderPlan render_plan_;
    engine::TimelineRenderSnapshot render_snapshot_;
    std::map<edit::Id, engine::TimelineMediaSource> metadata_cache_;
    qint64 render_origin_us_ = 0;
    int64_t audio_sample_ = 0;
    uint64_t audio_stream_generation_ = 0;
    uint64_t audio_session_generation_ = 1;
    uint64_t audio_control_revision_ = 0;
    bool audio_start_pending_ = false;
    QString audio_output_error_;
    edit::PlaybackInterval playback_bounds_;
    bool repeat_ = false;
    std::vector<uint64_t> solo_tracks_;
    uint64_t monitor_revision_ = 0;
    uint64_t latest_monitor_request_ = 0;
    std::shared_ptr<std::atomic<bool>> opening_cancelled_ = std::make_shared<std::atomic<bool>>(false);
    quint64 timeline_updates_ = 0;
    quint64 session_opens_ = 0;
    quint64 session_closes_ = 0;
    qint64 session_open_ns_ = 0;
    qint64 session_close_ns_ = 0;
    quint64 seek_dispatches_ = 0;
    quint64 video_seeks_ = 0;
    uint64_t last_loop_iteration_ = 0;
    qint64 seek_dispatch_ns_ = 0;
    qint64 timeline_queue_wait_ns_ = 0;
    qint64 seek_queue_wait_ns_ = 0;
    engine::EditPlayerSession::SeekPerformance retired_seek_performance_;
};

} // namespace exosnap::quick

Q_DECLARE_METATYPE(exosnap::engine::MonitorSelectionSnapshot)
