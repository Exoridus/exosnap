#pragma once

#include <QColor>
#include <QPoint>
#include <QString>
#include <QWidget>

class QLabel;
class QTimer;

namespace exosnap::ui::overlay {

// A small frameless, always-on-top, click-through status overlay window shown
// while recording (and paused). Displays a REC indicator dot + elapsed time.
//
// Capture exclusion:
//   SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE) is applied after the
//   window handle is created. If this call fails (e.g. unsupported OS build),
//   the overlay is hidden immediately and stays hidden for the session — the
//   overlay must never contaminate a recording.
//
// Positioning:
//   Default: top-right corner of the primary screen (or the recorded monitor when
//   provided via setMonitorGeometry). No drag/resize in this slice (v0.3.0).
class RecordingOverlayWindow : public QWidget {
    Q_OBJECT
  public:
    enum class OverlayState {
        Recording,
        Paused,
    };

    explicit RecordingOverlayWindow(QWidget* parent = nullptr);

    // Show/hide and transition between states.
    void showRecording(const QString& elapsed_text);
    void showPaused(const QString& elapsed_text);
    void updateElapsed(const QString& elapsed_text);
    void hideOverlay();

    // Set the geometry of the monitor being captured so the overlay can
    // position itself in the top-right corner of that monitor.
    // Pass a null/empty rect to fall back to the primary screen.
    void setMonitorGeometry(const QRect& monitor_rect);

    // Returns true if the display-affinity exclusion succeeded.
    // When false the overlay is hidden and will refuse to show.
    [[nodiscard]] bool isExcluded() const noexcept;

    [[nodiscard]] OverlayState overlayState() const noexcept;
    [[nodiscard]] const QString& elapsedText() const noexcept;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    void applyExclusion();
    void updatePosition();
    void applyState();

    OverlayState state_ = OverlayState::Recording;
    QString elapsed_text_;
    QRect monitor_rect_;    // virtual-screen geometry of the captured monitor
    bool excluded_ = false; // true when WDA_EXCLUDEFROMCAPTURE succeeded
    bool exclusion_attempted_ = false;

    // Blink state for the REC dot (paused: blinking; recording: solid red)
    QTimer* blink_timer_ = nullptr;
    bool blink_on_ = true;
};

} // namespace exosnap::ui::overlay
