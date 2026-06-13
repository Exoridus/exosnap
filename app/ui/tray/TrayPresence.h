#pragma once
#include <QObject>
#include <QString>
#include <QSystemTrayIcon>

class QAction;
class QMenu;

namespace exosnap::ui::tray {

// ---------------------------------------------------------------------------
// TrayIconState
// ---------------------------------------------------------------------------
// Canonical tray icon state — drives icon, tooltip, and menu item enable state.
// Idle is the default; Recording/Paused are mutually exclusive live states.

enum class TrayIconState {
    Idle,      // Not recording — ready or blocked
    Recording, // Active recording in progress (REC)
    Paused,    // Recording paused (PAUSED)
};

// ---------------------------------------------------------------------------
// TrayPresence
// ---------------------------------------------------------------------------
// Owns the QSystemTrayIcon and its context menu for the lifetime of the
// application.  Fed by the existing chromeStateChanged signal path in
// MainWindow: the owner calls applyState() on each state change.
//
// Design constraints (TRAY-PRESENCE-R1):
//  - Does NOT change window lifecycle (no minimize-to-tray / hide-on-close).
//  - Does NOT own timers; elapsed text, if shown, comes from the chromeRuntime-
//    MetricsChanged signal and is passed in as an optional string.
//  - Context-menu actions are wired to signals so the owner (MainWindow) can
//    route them through its existing action handlers — no duplicated logic.

class TrayPresence : public QObject {
    Q_OBJECT
  public:
    // parent must be the MainWindow (QWidget) so that activate/raise works.
    explicit TrayPresence(QObject* parent = nullptr);
    ~TrayPresence() override;

    // Apply the new tray state derived from the chrome state change.
    // status_label: uppercase trimmed label ("REC", "PAUSED", "READY", …).
    // elapsed_text: optional formatted elapsed string while recording; empty → omit.
    void applyState(TrayIconState state, const QString& status_label, const QString& elapsed_text = {});

    // Update only the elapsed text portion of the tooltip without changing the icon.
    // Call from chromeRuntimeMetricsChanged while state is Recording or Paused.
    void updateElapsedText(const QString& elapsed_text);

    // Reflect whether the main window is currently visible (for "Show/Hide" action label).
    void setWindowVisible(bool visible);

    // Reflect whether recording is blocked so the "Start recording" action is greyed out.
    void setRecordingBlocked(bool blocked);

    // Show/hide the icon.  The tray icon is always present while the app runs,
    // so this is called once during app startup (show) and once at quit (hide).
    void show();
    void hide();

    // ---- Unread notification badge (NOTIFY-SKIN-R1) ----------------------
    // Increment the unread count; updates the Notifications menu item label.
    // Call from MainWindow when an actionable toast becomes visible.
    void incrementUnreadCount();

    // Reset the unread count to zero; updates the menu item.
    // Call when the user focuses the window or opens the tray menu.
    void clearUnreadCount();

    // Returns the current unread count.
    [[nodiscard]] int unreadCount() const noexcept {
        return unread_count_;
    }

    // Read-only introspection for tests.
    [[nodiscard]] TrayIconState currentState() const {
        return state_;
    }
    [[nodiscard]] QString currentTooltip() const;

    // Direct action accessors for unit testing.
    [[nodiscard]] QAction* showHideAction() const {
        return show_hide_action_;
    }
    [[nodiscard]] QAction* recordToggleAction() const {
        return record_toggle_action_;
    }
    [[nodiscard]] QAction* notificationsAction() const {
        return notifications_action_;
    }

  signals:
    // Emitted when the user clicks "Show/Hide window" in the context menu or
    // double-clicks the tray icon.
    void activateWindowRequested();

    // Emitted when the user clicks "Start/Stop recording" — route via
    // MainWindow::recordToggleRequested to RecordPage (same path as hotkey).
    void recordToggleRequested();

    // Emitted when the user clicks "Quit" in the context menu.
    void quitRequested();

  private slots:
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void onShowHideTriggered();

  private:
    void rebuildTooltip();
    void applyIcon();

    void rebuildNotificationsLabel();

    QSystemTrayIcon* tray_icon_ = nullptr;
    QMenu* tray_menu_ = nullptr;
    QAction* show_hide_action_ = nullptr;
    QAction* record_toggle_action_ = nullptr;
    QAction* notifications_action_ = nullptr; // NOTIFY-SKIN-R1: unread badge mirror
    QAction* quit_action_ = nullptr;

    TrayIconState state_ = TrayIconState::Idle;
    QString status_label_;
    QString elapsed_text_;
    bool window_visible_ = true;
    bool recording_blocked_ = false;
    int unread_count_ = 0; // NOTIFY-SKIN-R1
};

// ---------------------------------------------------------------------------
// TrayPresenceStateMapper
// ---------------------------------------------------------------------------
// Pure function: map the chrome status label to a TrayIconState.
// Testable without a QApplication.

[[nodiscard]] TrayIconState TrayIconStateFromStatusLabel(const QString& status_label);

} // namespace exosnap::ui::tray
