#pragma once

#include <QWidget>

#include "notifications/NotificationEvent.h"

class QTimer;

namespace exosnap::notifications {
class NotificationManager;
}

namespace exosnap::ui::overlay {

// ---------------------------------------------------------------------------
// NotificationToastWindow — NOTIFY-TOASTS-R1
// ---------------------------------------------------------------------------
// Class-1 on-screen-only top-level window for transient notification toasts.
// Mirrors RecordingOverlayWindow for all mandatory class-1 properties:
//   - Qt::WindowTransparentForInput (click-through)
//   - SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)
//   - If exclusion fails → hide and stay hidden for the session
//
// Placement: bottom-right corner of the PRIMARY display (not the recorded
// monitor). Notifications are app-level events per ADR 0016.
//
// Visual: PLACEHOLDER QPainter pill stack. Final anatomy from NOTIFY-DESIGN-R1.
// Each toast shows a status-colored leading dot, title, and body line.
// Toasts stack vertically; the manager drives which events are visible.
//
// VISUAL PLACEHOLDER — final anatomy from NOTIFY-DESIGN-R1
//
class NotificationToastWindow : public QWidget {
    Q_OBJECT

  public:
    explicit NotificationToastWindow(notifications::NotificationManager* manager, QWidget* parent = nullptr);

    // Returns true if SetWindowDisplayAffinity succeeded.
    // When false the window is hidden and will refuse to show.
    [[nodiscard]] bool isExcluded() const noexcept;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    void applyExclusion();
    void updatePosition();
    void onVisibleSetChanged();

    notifications::NotificationManager* manager_ = nullptr; // non-owning
    bool excluded_ = false;
    bool exclusion_attempted_ = false;
};

} // namespace exosnap::ui::overlay
