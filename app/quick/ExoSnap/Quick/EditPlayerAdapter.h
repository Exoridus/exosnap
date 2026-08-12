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

  public:
    explicit EditPlayerAdapter(QObject* parent = nullptr);
    ~EditPlayerAdapter() override;

    void setSession(EditSessionAdapter* session);

    void attachPlayerItem(ExoEditPlayerItem* item);
    void detachPlayerItem(ExoEditPlayerItem* item);

    [[nodiscard]] bool playing() const noexcept;
    [[nodiscard]] bool clipOpen() const noexcept;
    [[nodiscard]] const QString& placeholderText() const noexcept;

    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void setPlaying(bool playing);
    // Scrubbing pauses; whether it resumes on release depends on whether the
    // preview was playing when the scrub began.
    Q_INVOKABLE void beginScrub();
    Q_INVOKABLE void endScrub();

  signals:
    void playingChanged();
    void clipOpenChanged();
    void placeholderTextChanged();

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
};

} // namespace exosnap::quick
