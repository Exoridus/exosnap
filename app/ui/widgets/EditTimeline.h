#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>
#include <cstdint>
#include <optional>
#include <vector>

#include "../../models/RecordingMarker.h"
#include "../../services/TimelineThumbnailSource.h"

class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QTimer;

namespace exosnap::ui::widgets {

// Interactive trim timeline for the Edit surface (custom-painted).
//
// A stack of rows under the player: one video row carrying decoded thumbnails
// of the clip, then one row per audio track. On top of all of them
//   - draggable trim handles at the in/out points (the trimmed-away ranges
//     are dimmed); handles constrain each other and never cross,
//   - marker lines rendered as thin secondary-accent verticals — deliberately
//     not the caution color, which the app reserves for a real diagnostic
//     warning,
//   - a playhead (white line + knob) that follows preview playback and can be
//     scrubbed by dragging,
//   - a centred time label above the active handle/playhead while dragging
//     ("MM:SS.mmm", hours only for recordings of one hour or longer),
//   - a static mono time row underneath (start · in/out readout · duration),
//   - a muted "Generating previews…" hint in the quiet zone above the stack
//     while the video row has fewer tiles than the current width can hold.
//
// Handles, markers and the playhead span every row: a trim applies to the whole
// clip, not to one of its tracks.
//
// Audio rows carry a label and a fill and nothing else. A peak envelope over a
// multi-hour recording means decoding the entire soundtrack, and an approximated
// one would be exactly the invented shape this widget stopped drawing.
//
// The widget owns only view/interaction state. Playback (the position clock)
// and trim snapping/persistence live in the page; the widget reports edits
// through signals and renders whatever it is given.
class EditTimeline : public QWidget {
    Q_OBJECT
  public:
    explicit EditTimeline(QWidget* parent = nullptr);
    ~EditTimeline() override;

    void setDurationMs(qint64 duration_ms);
    [[nodiscard]] qint64 durationMs() const noexcept {
        return duration_ms_;
    }

    void setMarkers(std::vector<RecordingMarker> markers);
    [[nodiscard]] const std::vector<RecordingMarker>& markers() const noexcept {
        return markers_;
    }

    // The clip the thumbnail strip is decoded from, plus its keyframe table
    // (the page already read it; re-reading here would open the file twice).
    // An empty path leaves the video row blank.
    void setClip(const QString& path, std::vector<int64_t> keyframes_us);

    // One row per audio track, top to bottom. An empty label is rendered
    // positionally ("Audio 1", "Audio 2") rather than guessed from track order,
    // which the container does not define. An empty list means no audio rows.
    void setAudioTrackLabels(const QStringList& labels);
    [[nodiscard]] const QStringList& audioTrackLabels() const noexcept {
        return audio_track_labels_;
    }

    // Replaces the strip with `count` deterministic placeholder tiles (-1 = as
    // many as the row holds, 0 = none). The strip normally comes from a real
    // decode; the visual harness and the widget tests have no clip, so they
    // inject instead. A count below the row's capacity renders the partly
    // filled state a real decode passes through.
    void setThumbnailFixture(int count);
    [[nodiscard]] int thumbnailCount() const noexcept {
        return static_cast<int>(thumbnails_.size());
    }

    // Trim range in clip time. Values are clamped to [0, duration] and to the
    // minimum handle gap. Full range (0, duration) means "no trim".
    void setTrimRangeMs(qint64 start_ms, qint64 end_ms);
    [[nodiscard]] qint64 trimStartMs() const noexcept {
        return trim_start_ms_;
    }
    [[nodiscard]] qint64 trimEndMs() const noexcept {
        return trim_end_ms_;
    }
    [[nodiscard]] bool isTrimmed() const noexcept {
        return duration_ms_ > 0 && (trim_start_ms_ > 0 || trim_end_ms_ < duration_ms_);
    }

    void setPositionMs(qint64 position_ms);
    [[nodiscard]] qint64 positionMs() const noexcept {
        return position_ms_;
    }

    // Pixel <-> time mapping over the track span (used by painting, hit
    // testing, and tests that verify proportional placement).
    [[nodiscard]] int xForMs(qint64 ms) const;
    [[nodiscard]] qint64 msForX(int x) const;

    // Row geometry, public so a caller can prepare a strip (or assert on the
    // stack's growth) without duplicating the layout maths.
    [[nodiscard]] int videoRowHeight() const noexcept;
    [[nodiscard]] int audioRowHeight() const noexcept;
    [[nodiscard]] int preferredHeight() const noexcept;

    // True while a handle or the playhead is being dragged (drives the scaled
    // handle + time-label feedback).
    [[nodiscard]] bool isDragActive() const noexcept {
        return drag_ != DragTarget::None;
    }

  signals:
    // Live while a trim handle is dragged (values already clamped).
    void trimRangeEdited(qint64 start_ms, qint64 end_ms);
    // Handle released — the owner may snap the values and write them back.
    void trimHandleReleased(qint64 start_ms, qint64 end_ms);
    // Scrub lifecycle: press on the playhead or the track, drag, release.
    void scrubStarted();
    void scrubMoved(qint64 position_ms);
    void scrubFinished();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    enum class DragTarget { None, TrimStart, TrimEnd, Playhead };

    // The whole row stack: handles, markers and the playhead span it, and it is
    // what xForMs()/msForX() map against.
    [[nodiscard]] QRect trackRect() const;
    [[nodiscard]] QRect videoRowRect() const;
    [[nodiscard]] QRect audioRowRect(int index) const;
    [[nodiscard]] QRect handleRect(qint64 ms, bool active) const;
    [[nodiscard]] DragTarget hitTest(const QPoint& pos) const;
    void updateHoverCursor(const QPoint& pos);

    // Re-derives the widget height from the current audio track count.
    void updateHeight();
    // Starts a decode run (or rebuilds the injected fixture) for the current
    // track width. Debounced through resize_debounce_: a resize changes the
    // tile count, and a drag across the window edge would otherwise queue one
    // run per intermediate width.
    void requestThumbnails();
    [[nodiscard]] std::vector<qint64> thumbnailTimesMs() const;

    qint64 duration_ms_ = 0;
    qint64 trim_start_ms_ = 0;
    qint64 trim_end_ms_ = 0;
    qint64 position_ms_ = 0;
    std::vector<RecordingMarker> markers_;
    QStringList audio_track_labels_;
    DragTarget drag_ = DragTarget::None;

    // Thumbnail strip. `thumbnail_run_` guards against tiles from a run whose
    // geometry the widget has already moved past.
    TimelineThumbnailSource* thumbnails_source_ = nullptr;
    std::vector<TimelineThumbnail> thumbnails_;
    std::vector<int64_t> keyframes_us_;
    QString clip_path_;
    quint64 thumbnail_run_ = 0;
    int clip_video_width_ = 0;
    int clip_video_height_ = 0;
    // Set while placeholder tiles stand in for a decode (-1 = fill the row).
    // Unset means the strip comes from the clip.
    std::optional<int> thumbnail_fixture_;
    QTimer* resize_debounce_ = nullptr;
};

} // namespace exosnap::ui::widgets
