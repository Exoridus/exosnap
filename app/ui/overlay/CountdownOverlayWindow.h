#pragma once

#include <QWidget>

// COUNTDOWN-OVERLAY-R1
//
// A class-1 (capture-excluded, click-through) countdown overlay shown on the
// recorded monitor during the pre-record countdown.  It mirrors the digit and
// ring-progress that the in-window TransportDock shows, keeping both perfectly
// in sync.
//
// Design tokens (Mappe wave03 CountdownOverlay SpecBox):
//   124px circle · bg rgba(12,12,14,0.74) · border rgba(255,255,255,0.16)
//   Ring: r=57, stroke 3px · track rgba(255,255,255,0.12)
//         progress #E6C57C (amber) · round linecap · starts 12 o'clock
//   Digit: IBM Plex Mono 52px weight-500 #E6C57C tabular-nums
//
// Centering: the window is always centered on the recorded monitor geometry
// supplied via setMonitorGeometry().
//
// Gating: the overlay gated behind show_recording_overlay (the same persisted
// setting that controls the REC status pill).  It does NOT introduce a new
// persisted setting.
//
// Exclusion: SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE) is applied
// once the native handle exists.  If the call fails the overlay stays hidden for
// the entire session.

namespace exosnap::ui::overlay {

class CountdownOverlayWindow : public QWidget {
    Q_OBJECT
  public:
    explicit CountdownOverlayWindow(QWidget* parent = nullptr);

    // Show the overlay for a running countdown.
    //   remaining_seconds : current displayed digit (1..duration_seconds)
    //   duration_seconds  : total countdown length used to compute ring progress
    void showCountdown(int remaining_seconds, int duration_seconds);

    // Notify of a tick (digit changed) without needing to re-show.
    void updateCountdown(int remaining_seconds, int duration_seconds);

    // Hide the overlay (countdown ended / cancelled / overlay disabled).
    void hideOverlay();

    // Set the geometry of the recorded monitor in virtual-screen coordinates.
    // Pass a null/empty rect to fall back to the primary screen.
    void setMonitorGeometry(const QRect& monitor_rect);

    // Returns true when WDA_EXCLUDEFROMCAPTURE succeeded.
    // When false the overlay stays hidden.
    [[nodiscard]] bool isExcluded() const noexcept;

    [[nodiscard]] int remainingSeconds() const noexcept;
    [[nodiscard]] int durationSeconds() const noexcept;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    void applyExclusion();
    void updatePosition();

    int remaining_seconds_ = 3;
    int duration_seconds_ = 3;
    QRect monitor_rect_;
    bool excluded_ = false;
    bool exclusion_attempted_ = false;
};

} // namespace exosnap::ui::overlay
