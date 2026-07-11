#pragma once

#include <QWidget>
#include <vector>

#include "../../models/RecordingMarker.h"

class QMouseEvent;
class QPaintEvent;

namespace exosnap::ui::widgets {

// Interactive trim timeline for the Edit surface (custom-painted).
//
// One strip under the player: a pseudo-waveform track with
//   - draggable trim handles at the in/out points (the trimmed-away ranges
//     are dimmed); handles constrain each other and never cross,
//   - marker lines rendered exactly like the record timeline in the design
//     suite (thin caution-coloured verticals),
//   - a playhead (white line + knob) that follows preview playback and can be
//     scrubbed by dragging,
//   - a centred time label above the active handle/playhead while dragging
//     ("MM:SS.mmm", hours only for recordings of one hour or longer),
//   - a static mono time row underneath (start · in/out readout · duration).
//
// The widget owns only view/interaction state. Playback (the position clock)
// and trim snapping/persistence live in the page; the widget reports edits
// through signals and renders whatever it is given.
class EditTimeline : public QWidget {
    Q_OBJECT
  public:
    explicit EditTimeline(QWidget* parent = nullptr);

    void setDurationMs(qint64 duration_ms);
    [[nodiscard]] qint64 durationMs() const noexcept {
        return duration_ms_;
    }

    void setMarkers(std::vector<RecordingMarker> markers);
    [[nodiscard]] const std::vector<RecordingMarker>& markers() const noexcept {
        return markers_;
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

  private:
    enum class DragTarget { None, TrimStart, TrimEnd, Playhead };

    [[nodiscard]] QRect trackRect() const;
    [[nodiscard]] QRect handleRect(qint64 ms, bool active) const;
    [[nodiscard]] DragTarget hitTest(const QPoint& pos) const;
    void updateHoverCursor(const QPoint& pos);

    qint64 duration_ms_ = 0;
    qint64 trim_start_ms_ = 0;
    qint64 trim_end_ms_ = 0;
    qint64 position_ms_ = 0;
    std::vector<RecordingMarker> markers_;
    DragTarget drag_ = DragTarget::None;
};

} // namespace exosnap::ui::widgets
