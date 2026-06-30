#pragma once

#include <QPoint>
#include <QSize>
#include <QWidget>

// QUICK-PILL-R1: opt-in interactive capture-excluded control pill.
//
// Architecture:
//   This is a NEW overlay category — capture-excluded (WDA_EXCLUDEFROMCAPTURE)
//   BUT interactive (accepts mouse clicks and drags). It is explicitly NOT a
//   class-1 element: class-1 elements MUST be click-through
//   (Qt::WindowTransparentForInput). This pill is in its own interactive window
//   class. See ADR 0016.
//
//   - Apply SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE) with the
//     hide-on-failure guard (same as RecordingOverlayWindow).
//   - Do NOT set Qt::WindowTransparentForInput — this window accepts input.
//
// Design spec (Mappe §0.3 / QuickPill):
//   - Background: rgba(12,12,14,0.8)
//   - Border: rgba(255,255,255,0.16), radius 16
//   - Drop shadow: approximated by opaque dark bg + dark box-shadow
//   - Grip handle: 28px wide, grip glyph rgba(255,255,255,0.5)
//   - Clicking grip toggles collapse/expand
//   - Dragging grip repositions the window
//   - Collapsed = grip handle only; expanded = grip + action buttons
//
// Buttons (only real backed actions — no fake Marker button):
//   - Pause/Resume — emits pauseResumeRequested()
//   - Stop         — emits stopRequested()  [rec-styled: coral]
//   - Capture frame— emits captureFrameRequested()
//
//   MARKER button: deferred to 0.11.0 (markers wave) — NOT rendered.
//
// Gating:
//   - show_quick_controls setting must be true AND a recording must be active.
//   - updateState() drives visibility; MainWindow calls it on recording state changes.

class QMouseEvent;
class QPaintEvent;

namespace exosnap::ui::overlay {

class QuickControlPillWindow : public QWidget {
    Q_OBJECT

  public:
    explicit QuickControlPillWindow(QWidget* parent = nullptr);

    // State: paused is used to switch the pause/resume button glyph.
    void updateState(bool recording_active, bool paused);

    // Visibility gate: show only when show_quick_controls is on AND recording is active.
    void setShowQuickControls(bool show);

    // Returns true if WDA_EXCLUDEFROMCAPTURE succeeded.
    [[nodiscard]] bool isExcluded() const noexcept;

    // Collapse/expand the pill (grip-click toggles; external call available for tests).
    void setExpanded(bool expanded);
    [[nodiscard]] bool isExpanded() const noexcept;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  signals:
    void pauseResumeRequested();
    void stopRequested();
    void captureFrameRequested();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void applyExclusion();
    void updatePosition();
    void updateVisibility();

    // Hit-test helpers (all in local widget coords)
    [[nodiscard]] QRect gripRect() const;
    [[nodiscard]] QRect pauseResumeButtonRect() const;
    [[nodiscard]] QRect stopButtonRect() const;
    [[nodiscard]] QRect captureFrameButtonRect() const;

    bool show_quick_controls_ = false;
    bool recording_active_ = false;
    bool paused_ = false;
    bool expanded_ = true;
    bool excluded_ = false;
    bool exclusion_attempted_ = false;

    // Drag state
    bool dragging_ = false;
    QPoint drag_start_global_; // global screen position where drag started
};

} // namespace exosnap::ui::overlay
