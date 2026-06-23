#pragma once

#include <QRectF>
#include <QVector>
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

    // ── Shared hit-test geometry (paint + mouse never drift) ──────────────────
    // One clickable target inside a rendered toast: either the dismiss ✕ or one
    // action pill. `is_dismiss` selects which; `action` is meaningful for pills.
    struct ToastHit {
        QRectF rect;             // window-space hit rect
        uint64_t sequence = 0;   // owning event
        bool is_dismiss = false; // ✕ vs. action pill
        notifications::NotificationAction action = notifications::NotificationAction::None;
    };

    // Computes every clickable target for the current visible set, in the exact
    // window-space coordinates used by paintEvent(). Public + static-friendly so
    // unit tests can assert the geometry without a live window.
    [[nodiscard]] QVector<ToastHit> computeHitTargets() const;

  signals:
    // Emitted when the user clicks an action pill inside a toast. MainWindow owns
    // the dispatch (Open folder / Show file / Recover / …) — the toast never runs
    // action logic itself, it only reuses the same handler the hub/recovery use.
    void actionTriggered(notifications::NotificationEvent event, notifications::NotificationAction action);

  protected:
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

  private:
    void applyExclusion();
    void updatePosition();
    void updateMask();
    void onVisibleSetChanged();

    notifications::NotificationManager* manager_ = nullptr; // non-owning
    bool excluded_ = false;
    bool exclusion_attempted_ = false;

    // ~30 fps repaint while non-sticky toasts are visible, so the countdown bar
    // animates smoothly. Stopped whenever only sticky toasts (or none) remain.
    QTimer* repaint_timer_ = nullptr;
};

} // namespace exosnap::ui::overlay
