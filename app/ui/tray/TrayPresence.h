#pragma once
#include <QObject>
#include <QString>
#include <QSystemTrayIcon>

#include "models/ShellPresence.h"

class QAction;
class QMenu;

namespace exosnap::ui::tray {

// ---------------------------------------------------------------------------
// TrayPresence
// ---------------------------------------------------------------------------
// Owns the QSystemTrayIcon and its context menu for the lifetime of the
// application.
//
// It decides nothing. What the menu offers and which icon is shown come from the
// shell projection (models/ShellPresence.h), which the taskbar's thumbnail
// buttons read as well -- one table, two renderings. The tray used to re-derive
// its state by parsing the status label, which is presentation and may be
// localized.
//
// Design constraints (TRAY-PRESENCE-R1):
//  - Does NOT change window lifecycle (no minimize-to-tray / hide-on-close).
//  - Does NOT own timers. Both the elapsed text and the recording heartbeat's
//    frame are pushed in, so the tray cannot drift from the taskbar badge.
//  - Menu actions are wired to signals so the owner routes them through its
//    existing handlers -- no duplicated logic.

class TrayPresence : public QObject {
    Q_OBJECT
  public:
    explicit TrayPresence(QObject* parent = nullptr);
    ~TrayPresence() override;

    // Apply the shell projection.
    //
    // `elapsed_text` is the formatted elapsed string while recording; empty
    // omits it. `pulse_frame` indexes the recording heartbeat and is ignored in
    // every state but Recording.
    void applyState(const ShellPresenceState& state, const QString& elapsed_text = {}, int pulse_frame = 0);

    // Update only the elapsed text portion of the tooltip without touching the
    // icon. Called on the runtime-metrics cadence, which is far denser than the
    // state changes.
    void updateElapsedText(const QString& elapsed_text);

    // Reflect whether the main window is currently visible (for "Show/Hide" action label).
    void setWindowVisible(bool visible);

    // Show/hide the icon.  The tray icon is always present while the app runs,
    // so this is called once during app startup (show) and once at quit (hide).
    void show();
    void hide();

    // ---- Unread notification badge (NOTIFY-SKIN-R1) ----------------------
    // Increment the unread count; updates the Notifications menu item label.
    // Call from the shell when an actionable toast becomes visible.
    void incrementUnreadCount();

    // Reset the unread count to zero; updates the menu item.
    // Call when the user focuses the window or opens the tray menu.
    void clearUnreadCount();

    // Returns the current unread count.
    [[nodiscard]] int unreadCount() const noexcept {
        return unread_count_;
    }

    // Read-only introspection for tests.
    [[nodiscard]] ShellIconState currentIconState() const noexcept {
        return state_.icon_state;
    }
    [[nodiscard]] QString currentTooltip() const;
    // Which pre-rendered heartbeat frame the icon is currently showing.
    [[nodiscard]] int currentPulseFrame() const noexcept {
        return pulse_frame_;
    }

    // Direct action accessors for unit testing.
    [[nodiscard]] QAction* showHideAction() const {
        return show_hide_action_;
    }
    [[nodiscard]] QAction* recordAction() const {
        return record_action_;
    }
    [[nodiscard]] QAction* pauseResumeAction() const {
        return pause_resume_action_;
    }
    [[nodiscard]] QAction* stopAction() const {
        return stop_action_;
    }
    [[nodiscard]] QAction* notificationsAction() const {
        return notifications_action_;
    }

  signals:
    // Emitted when the user asks for the window while it is NOT on screen --
    // "Show window" in the context menu, or a double-click on the tray icon.
    void activateWindowRequested();

    // Emitted when the user clicks the same context-menu entry while the window
    // IS on screen, where it reads "Hide window". Its own signal rather than a
    // second meaning for the one above: that entry used to raise the window under
    // both labels, so the menu offered to hide a window and then showed it.
    void hideWindowRequested();

    // A transport entry was chosen. The same signal the taskbar's thumbnail
    // buttons raise, carrying the same projection-resolved intent.
    void shellActionRequested(ShellAction action);

    // A double-click on the icon, which is "toggle recording" rather than a
    // specific transport action -- the gesture has no state to read.
    void recordToggleRequested();

    // Emitted when the user clicks "Quit" in the context menu.
    void quitRequested();

  private slots:
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void onShowHideTriggered();

  private:
    void rebuildTooltip();
    void applyIcon();
    void applyMenuState();
    void rebuildNotificationsLabel();
    // Raises `action` only when the projection allows it. A menu item can be
    // triggered by an accelerator between the state change and the repaint.
    void requestAction(ShellButton button);

    QSystemTrayIcon* tray_icon_ = nullptr;
    QMenu* tray_menu_ = nullptr;
    QAction* show_hide_action_ = nullptr;
    QAction* record_action_ = nullptr;
    // One entry, two labels -- the same reason the taskbar spends one thumbnail
    // slot on it: the two are mutually exclusive.
    QAction* pause_resume_action_ = nullptr;
    QAction* stop_action_ = nullptr;
    QAction* notifications_action_ = nullptr; // NOTIFY-SKIN-R1: unread badge mirror
    QAction* quit_action_ = nullptr;

    ShellPresenceState state_;
    QString elapsed_text_;
    int pulse_frame_ = 0;
    // False until the first applyState() call, so the initial state is written
    // through even when it equals the member defaults.
    bool state_applied_ = false;
    bool window_visible_ = true;
    int unread_count_ = 0; // NOTIFY-SKIN-R1
};

} // namespace exosnap::ui::tray
