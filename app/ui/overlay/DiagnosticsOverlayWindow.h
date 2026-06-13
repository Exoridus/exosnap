#pragma once

#include <QColor>
#include <QRect>
#include <QString>
#include <QWidget>

namespace exosnap::ui::overlay {

// A compact, frameless, always-on-top, click-through diagnostics overlay shown
// while recording (and paused). Displays fps/bitrate, A/V drift, dropped frames,
// output file size, and muted-source glyphs (mic/sys).
//
// Capture exclusion:
//   SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE) is applied after the
//   window handle is created. If this call fails (e.g. unsupported OS build),
//   the overlay is hidden immediately and stays hidden for the session — the
//   overlay must never contaminate a recording.
//
// Positioning:
//   Bottom-right corner of the recorded monitor (or primary screen when no
//   monitor rect is set). RecordingOverlayWindow occupies the top-right corner;
//   this window sits below it to avoid overlap.
//
// Default: OFF (controlled by PersistedAppSettings::show_diagnostics_overlay).
class DiagnosticsOverlayWindow : public QWidget {
    Q_OBJECT
  public:
    explicit DiagnosticsOverlayWindow(QWidget* parent = nullptr);

    // Show the overlay (recording or paused state).
    void showOverlay();
    // Hide the overlay.
    void hideOverlay();

    // Update live metrics. Does not show the overlay on its own.
    void updateMetrics(const QString& fps_bitrate_text, const QString& av_drift_text,
                       const QString& dropped_frames_text, const QString& output_size_text, bool mic_muted,
                       bool sys_muted);

    // Set the geometry of the monitor being captured so the overlay can
    // position itself in the bottom-right corner of that monitor.
    // Pass a null/empty rect to fall back to the primary screen.
    void setMonitorGeometry(const QRect& monitor_rect);

    // Returns true if the display-affinity exclusion succeeded.
    // When false the overlay is hidden and will refuse to show.
    [[nodiscard]] bool isExcluded() const noexcept;

    // Metric state accessors (for testing).
    [[nodiscard]] const QString& fpsBitrateText() const noexcept;
    [[nodiscard]] const QString& avDriftText() const noexcept;
    [[nodiscard]] const QString& droppedFramesText() const noexcept;
    [[nodiscard]] const QString& outputSizeText() const noexcept;
    [[nodiscard]] bool isMicMuted() const noexcept;
    [[nodiscard]] bool isSysMuted() const noexcept;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    void applyExclusion();
    void updatePosition();

    // Metric fields
    QString fps_bitrate_text_;
    QString av_drift_text_;
    QString dropped_frames_text_;
    QString output_size_text_;
    bool mic_muted_ = false;
    bool sys_muted_ = false;

    QRect monitor_rect_;    // virtual-screen geometry of the captured monitor
    bool excluded_ = false; // true when WDA_EXCLUDEFROMCAPTURE succeeded
    bool exclusion_attempted_ = false;
};

} // namespace exosnap::ui::overlay
