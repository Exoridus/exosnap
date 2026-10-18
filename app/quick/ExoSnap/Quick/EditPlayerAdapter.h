#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <exosnap/engine/edit_player_session.h>
#include <exosnap/engine/edit_timeline_compositor.h>

#include "models/EditWorkspace.h"
#include <atomic>
#include <memory>
#include <mutex>

namespace exosnap::quick {

class EditPlayerWorker;
class EditSessionAdapter;
class ExoEditPlayerItem;

// Presentation cadence for a clip at `clip_fps`, capped at `screen_hz`. Painting
// more often than the display refreshes cannot show more motion, it only burns a
// convert+blit per wasted tick. Pure, so the cap is testable without a clip.
[[nodiscard]] int EditPreviewTickMsFor(double clip_fps, double screen_hz) noexcept;

// Keeps the decoder's frame callback from reaching a destroyed item. The
// callback fires on the decode/seek thread; attach/detach happen on the GUI
// thread. Shared by value into the callback, so the sink outlives both.
class EditPlayerFrameSink {
  public:
    void attach(ExoEditPlayerItem* item);
    void detach(const ExoEditPlayerItem* item);
    void deliver(exosnap::engine::RawDecodedVideoFrame frame);
    void deliver(exosnap::engine::RawDecodedVideoFrame frame, uint64_t generation, int64_t timeline_offset_us = 0);
    void deliverTimeline(exosnap::engine::TimelineVideoFrame frame, uint64_t generation);
    uint64_t invalidate();
    void publishClock(int64_t media_time_us);
    void clear();

  private:
    std::mutex mutex_;
    ExoEditPlayerItem* item_ = nullptr;
    uint64_t generation_ = 0;
};

// Owns the decoder session and its pacing, on a thread of its own.
//
// EditPlayerSession documents Open/Close/Play/Pause/SeekTo as single-caller-
// thread calls, and the Widgets surface satisfied that by making the GUI thread
// the caller -- which is why opening a clip blocked it. Here the "caller thread"
// is a dedicated worker: the contract is still satisfied, and Open, the pacing
// tick and every seek stay off the GUI thread. Decoded frames never touch this
// class; they go straight from the decode thread into the scene-graph item.
class EditPlayerAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("EditPlayerAdapter is provided by the application")

    Q_PROPERTY(QUrl harnessStillSource READ harnessStillSource NOTIFY harnessStillSourceChanged FINAL)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged FINAL)
    Q_PROPERTY(PreviewState previewState READ previewState NOTIFY previewChanged FINAL)
    Q_PROPERTY(qint64 requestedTimeUs READ requestedTimeUs NOTIFY previewChanged FINAL)
    Q_PROPERTY(QVariant presentedTimeUs READ presentedTimeUs NOTIFY previewChanged FINAL)
    Q_PROPERTY(bool seekPending READ seekPending NOTIFY previewChanged FINAL)
    Q_PROPERTY(bool buffering READ buffering NOTIFY previewChanged FINAL)
    Q_PROPERTY(QVariant actuallyDroppedRecent READ actuallyDroppedRecent NOTIFY previewChanged FINAL)
    Q_PROPERTY(bool clipOpen READ clipOpen NOTIFY clipOpenChanged FINAL)
    // A scrub is in progress. Distinct from `playing`, which a scrub forces
    // false: the surface has to be able to tell "the user is dragging the
    // playhead" from "the preview is paused", because what it draws over the
    // picture differs between the two.
    Q_PROPERTY(bool scrubbing READ scrubbing NOTIFY scrubbingChanged FINAL)
    Q_PROPERTY(bool desiredPlaying READ desiredPlaying NOTIFY transportChanged FINAL)
    Q_PROPERTY(bool transportPending READ transportPending NOTIFY transportChanged FINAL)
    Q_PROPERTY(QString placeholderText READ placeholderText NOTIFY placeholderTextChanged FINAL)
    // Is the Edit workspace the surface the user is looking at? Bound by the
    // shell from the navigation state (QCR-001) -- the same shape
    // RecordPreviewAdapter::surfaceVisible has, and for the same reason: the
    // FACT is the shell's, the POLICY is here.
    Q_PROPERTY(bool surfaceVisible READ surfaceVisible WRITE setSurfaceVisible NOTIFY surfaceVisibleChanged FINAL)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged FINAL)
    Q_PROPERTY(QVariantList monitoringTracks READ monitoringTracks NOTIFY monitoringChanged FINAL)
    Q_PROPERTY(bool monitoringConfirmationRequired READ monitoringConfirmationRequired NOTIFY monitoringChanged FINAL)

  public:
    explicit EditPlayerAdapter(QObject* parent = nullptr);
    ~EditPlayerAdapter() override;
    enum PreviewState { Empty, Opening, Ready, SeekPending, Buffering, Degraded, Failed };
    Q_ENUM(PreviewState)
    PreviewState previewState() const {
        return preview_state_;
    }
    qint64 requestedTimeUs() const {
        return requested_time_us_;
    }
    QVariant presentedTimeUs() const;
    bool seekPending() const {
        return seek_pending_;
    }
    bool buffering() const {
        return preview_state_ == Buffering;
    }
    QVariant actuallyDroppedRecent() const {
        return {};
    }

    void setSession(EditSessionAdapter* session);
    // Cumulative time spent copying workspaces for the playback worker.
    [[nodiscard]] qint64 workspaceCopyTimeNs() const {
        return workspace_copy_ns_;
    }

    void attachPlayerItem(ExoEditPlayerItem* item);
    void detachPlayerItem(ExoEditPlayerItem* item);

    [[nodiscard]] bool playing() const noexcept;
    bool desiredPlaying() const noexcept {
        return desired_playing_;
    }
    bool transportPending() const noexcept {
        return transport_pending_;
    }
    [[nodiscard]] bool clipOpen() const noexcept;
    [[nodiscard]] bool scrubbing() const noexcept;
    [[nodiscard]] const QString& placeholderText() const noexcept;
    [[nodiscard]] bool surfaceVisible() const noexcept;

    // The workspace left the screen, or came back. Leaving PAUSES: video and
    // audio out of a surface the user is not looking at is both surprising on
    // another page and decoder work nobody asked for. It does NOT seek, does not
    // close the clip and does not end the session -- the position stands, and
    // coming back leaves the preview paused where it was. Resuming is the user's
    // own action, so there is deliberately nothing to do when this turns true.
    void setSurfaceVisible(bool visible);
    double volume() const {
        return volume_;
    }
    void setVolume(double volume);

    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void setPlaying(bool playing);
    Q_INVOKABLE void confirmMonitoring(QStringList trackIds, bool overlapConfirmed);
    QVariantList monitoringTracks() const {
        return monitoring_tracks_;
    }
    bool monitoringConfirmationRequired() const {
        return monitoring_confirmation_required_;
    }
    void setMonitoringState(QVariantList tracks, bool confirmationRequired);
    // Scrubbing quiesces playback without changing the requested intent.
    // Explicit Play/Pause during the gesture determines its release behavior.
    Q_INVOKABLE void beginScrub();
    Q_INVOKABLE void endScrub();

    // Test seam. Puts the adapter in the state a successfully opened clip leaves
    // it in, without a file, a decoder or a worker round trip -- the worker
    // refuses every call while it holds no session, so nothing downstream runs.
    // Named like EditSessionAdapter::setKeyframeTimestampsForTest.
    void setClipStateForTest(bool clip_open, qint64 duration_ms);

    // Synthetic composition for --visual-test, independent of the native
    // decoder and renderer. Does not publish a native presentation timestamp.
    void showStillForHarness(const QUrl& source, qint64 time_us, qint64 duration_ms);
    const QUrl& harnessStillSource() const {
        return harness_still_source_;
    }

  signals:
    void harnessStillSourceChanged();
    void previewChanged();
    void playingChanged();
    void clipOpenChanged();
    void scrubbingChanged();
    void placeholderTextChanged();
    void surfaceVisibleChanged();
    void volumeChanged();
    void monitoringChanged();
    void transportChanged();

  private:
    friend class EditPlayerAdapterTestPeer;
    void openClip(const QString& master_path, qint64 duration_ms);
    void closeClip();
    void seekUs(qint64 position_us);
    void dispatchSeek();
    void dispatchTransport(bool seek);
    void setPreviewState(PreviewState state);
    void setPlaceholderText(const QString& text);
    [[nodiscard]] double currentScreenRefreshHz() const;

    EditSessionAdapter* session_ = nullptr;
    QThread worker_thread_;
    EditPlayerWorker* worker_ = nullptr;
    std::shared_ptr<EditPlayerFrameSink> sink_ = std::make_shared<EditPlayerFrameSink>();
    QPointer<ExoEditPlayerItem> item_;
    QString placeholder_text_ = QStringLiteral("Preview unavailable");
    qint64 duration_ms_ = 0;
    bool playing_ = false;
    bool desired_playing_ = false;
    bool transport_pending_ = false;
    bool clip_open_ = false;
    bool scrubbing_ = false;
    // True until the shell says otherwise: a session that is handed a clip is
    // handed it on Record, which is the page the workspace is visible on.
    bool surface_visible_ = true;
    double volume_ = 1.0;
    std::optional<edit::Revision> timeline_revision_;
    uint64_t timeline_monitoring_revision_ = 0;
    qint64 workspace_copy_ns_ = 0;
    std::shared_ptr<std::atomic<bool>> pending_timeline_current_;
    struct SeekRequest {
        qint64 time_us = 0;
        uint64_t generation = 0;
        uint64_t command = 0;
        bool playing = false;
    };
    std::optional<SeekRequest> pending_seek_;
    bool seek_dispatch_active_ = false;
    uint64_t request_generation_ = 0;
    uint64_t transport_command_ = 0;
    uint64_t monitoring_request_ = 0;
    std::shared_ptr<std::atomic<uint64_t>> current_transport_ = std::make_shared<std::atomic<uint64_t>>(0);
    PreviewState preview_state_ = Empty;
    qint64 requested_time_us_ = 0;
    std::optional<qint64> presented_time_us_;
    bool seek_pending_ = false;
    QVariantList monitoring_tracks_;
    bool monitoring_confirmation_required_ = false;
    QUrl harness_still_source_;
};

} // namespace exosnap::quick
