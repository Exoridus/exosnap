#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

#include <recorder_core/edit_player_session.h>

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
    void deliver(recorder_core::RawDecodedVideoFrame frame);
    void publishClock(int64_t media_time_us);
    void clear();

  private:
    std::mutex mutex_;
    ExoEditPlayerItem* item_ = nullptr;
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

    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged FINAL)
    Q_PROPERTY(bool clipOpen READ clipOpen NOTIFY clipOpenChanged FINAL)
    Q_PROPERTY(QString placeholderText READ placeholderText NOTIFY placeholderTextChanged FINAL)
    // Is the Edit workspace the surface the user is looking at? Bound by the
    // shell from the navigation state (QCR-001) -- the same shape
    // RecordPreviewAdapter::surfaceVisible has, and for the same reason: the
    // FACT is the shell's, the POLICY is here.
    Q_PROPERTY(bool surfaceVisible READ surfaceVisible WRITE setSurfaceVisible NOTIFY surfaceVisibleChanged FINAL)

  public:
    explicit EditPlayerAdapter(QObject* parent = nullptr);
    ~EditPlayerAdapter() override;

    void setSession(EditSessionAdapter* session);

    void attachPlayerItem(ExoEditPlayerItem* item);
    void detachPlayerItem(ExoEditPlayerItem* item);

    [[nodiscard]] bool playing() const noexcept;
    [[nodiscard]] bool clipOpen() const noexcept;
    [[nodiscard]] const QString& placeholderText() const noexcept;
    [[nodiscard]] bool surfaceVisible() const noexcept;

    // The workspace left the screen, or came back. Leaving PAUSES: video and
    // audio out of a surface the user is not looking at is both surprising on
    // another page and decoder work nobody asked for. It does NOT seek, does not
    // close the clip and does not end the session -- the position stands, and
    // coming back leaves the preview paused where it was. Resuming is the user's
    // own action, so there is deliberately nothing to do when this turns true.
    void setSurfaceVisible(bool visible);

    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void setPlaying(bool playing);
    // Scrubbing pauses; whether it resumes on release depends on whether the
    // preview was playing when the scrub began.
    Q_INVOKABLE void beginScrub();
    Q_INVOKABLE void endScrub();

    // Test seam. Puts the adapter in the state a successfully opened clip leaves
    // it in, without a file, a decoder or a worker round trip -- the worker
    // refuses every call while it holds no session, so nothing downstream runs.
    // Named like EditSessionAdapter::setKeyframeTimestampsForTest.
    void setClipStateForTest(bool clip_open, qint64 duration_ms);

  signals:
    void playingChanged();
    void clipOpenChanged();
    void placeholderTextChanged();
    void surfaceVisibleChanged();

  private:
    void openClip(const QString& master_path, qint64 duration_ms);
    void closeClip();
    void seek(qint64 position_ms);
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
    bool clip_open_ = false;
    bool resume_after_scrub_ = false;
    // True until the shell says otherwise: a session that is handed a clip is
    // handed it on Record, which is the page the workspace is visible on.
    bool surface_visible_ = true;
};

} // namespace exosnap::quick
