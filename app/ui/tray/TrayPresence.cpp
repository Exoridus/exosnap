#include "TrayPresence.h"

#include <QAction>
#include <QFile>
#include <QIcon>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QWidget>

namespace exosnap::ui::tray {

// ---------------------------------------------------------------------------
// TrayPresenceStateMapper
// ---------------------------------------------------------------------------

TrayIconState TrayIconStateFromStatusLabel(const QString& status_label) {
    const QString upper = status_label.trimmed().toUpper();
    if (upper == QStringLiteral("PAUSED"))
        return TrayIconState::Paused;
    if (upper == QStringLiteral("REC") || upper == QStringLiteral("RECORDING") || upper == QStringLiteral("STARTING") ||
        upper == QStringLiteral("COUNTDOWN"))
        return TrayIconState::Recording;
    return TrayIconState::Idle;
}

// ---------------------------------------------------------------------------
// TrayPresence
// ---------------------------------------------------------------------------

TrayPresence::TrayPresence(QObject* parent) : QObject(parent) {
    // tray_icon_ is parented to this (QObject), so it will be destroyed
    // automatically.  tray_menu_ is parented to tray_icon_ so it is in the
    // QObject tree and findChild<QMenu*>() works from TrayPresence in tests.
    tray_icon_ = new QSystemTrayIcon(this);

    // Parent the menu to tray_icon_ so the QObject tree owns it.
    tray_menu_ = new QMenu();
    tray_menu_->setParent(nullptr); // unparented widget; owned by tray_icon_ via setContextMenu below

    show_hide_action_ = tray_menu_->addAction(QStringLiteral("Show window"));
    record_toggle_action_ = tray_menu_->addAction(QStringLiteral("Start recording"));
    tray_menu_->addSeparator();
    // NOTIFY-SKIN-R1: clickable mirror for over-game toasts.
    // Clicking this focuses/shows the ExoSnap window (the spec's named mechanism).
    // Label is updated as "Notifications (N)" when N > 0, else hidden.
    notifications_action_ = tray_menu_->addAction(QStringLiteral("Notifications"));
    notifications_action_->setVisible(false); // hidden until there are unread items
    tray_menu_->addSeparator();
    quit_action_ = tray_menu_->addAction(QStringLiteral("Quit ExoSnap"));

    connect(show_hide_action_, &QAction::triggered, this, &TrayPresence::onShowHideTriggered);
    connect(record_toggle_action_, &QAction::triggered, this, &TrayPresence::recordToggleRequested);
    // Notifications action: clicking focuses the window and clears the badge
    // (MainWindow wires clearUnreadCount via activateWindowRequested).
    connect(notifications_action_, &QAction::triggered, this, [this]() {
        clearUnreadCount();
        emit activateWindowRequested();
    });
    connect(quit_action_, &QAction::triggered, this, &TrayPresence::quitRequested);

    tray_icon_->setContextMenu(tray_menu_);

    connect(tray_icon_, &QSystemTrayIcon::activated, this, &TrayPresence::onTrayActivated);

    applyIcon();
    rebuildTooltip();
}

TrayPresence::~TrayPresence() {
    // tray_menu_ is a QWidget with no parent; delete it explicitly before
    // tray_icon_ (which does not own the menu despite holding a pointer).
    delete tray_menu_;
    tray_menu_ = nullptr;
}

void TrayPresence::applyState(TrayIconState state, const QString& status_label, const QString& elapsed_text) {
    state_ = state;
    status_label_ = status_label;
    elapsed_text_ = elapsed_text;

    applyIcon();
    rebuildTooltip();

    // Update the "Start/Stop recording" menu item label.
    if (record_toggle_action_) {
        switch (state_) {
        case TrayIconState::Recording:
            record_toggle_action_->setText(QStringLiteral("Stop recording"));
            break;
        case TrayIconState::Paused:
            record_toggle_action_->setText(QStringLiteral("Stop recording"));
            break;
        case TrayIconState::Idle:
        default:
            record_toggle_action_->setText(QStringLiteral("Start recording"));
            break;
        }
        // Disable "Start recording" when idle+blocked; always enabled while recording/paused.
        const bool can_toggle = (state_ != TrayIconState::Idle) || !recording_blocked_;
        record_toggle_action_->setEnabled(can_toggle);
    }
}

void TrayPresence::updateElapsedText(const QString& elapsed_text) {
    elapsed_text_ = elapsed_text;
    rebuildTooltip();
}

void TrayPresence::setWindowVisible(bool visible) {
    window_visible_ = visible;
    if (show_hide_action_)
        show_hide_action_->setText(visible ? QStringLiteral("Hide window") : QStringLiteral("Show window"));
}

void TrayPresence::setRecordingBlocked(bool blocked) {
    recording_blocked_ = blocked;
    if (record_toggle_action_ && state_ == TrayIconState::Idle)
        record_toggle_action_->setEnabled(!blocked);
}

void TrayPresence::show() {
    if (tray_icon_)
        tray_icon_->show();
}

void TrayPresence::hide() {
    if (tray_icon_)
        tray_icon_->hide();
}

void TrayPresence::showMessage(const QString& title, const QString& message, QSystemTrayIcon::MessageIcon icon,
                               int msecs) {
    if (tray_icon_ && tray_icon_->isVisible())
        tray_icon_->showMessage(title, message, icon, msecs);
}

QString TrayPresence::currentTooltip() const {
    // Tooltip format per Mappe "Tray behavior" SpecBox:
    //   "ExoSnap — Ready"
    //   "ExoSnap — Recording 04:17"
    //   "ExoSnap — Paused"
    QString tip = QStringLiteral("ExoSnap — ");

    switch (state_) {
    case TrayIconState::Recording:
        tip += QStringLiteral("Recording");
        if (!elapsed_text_.isEmpty())
            tip += QLatin1Char(' ') + elapsed_text_;
        break;
    case TrayIconState::Paused:
        tip += QStringLiteral("Paused");
        break;
    case TrayIconState::Idle:
    default:
        tip += QStringLiteral("Ready");
        break;
    }

    return tip;
}

void TrayPresence::rebuildTooltip() {
    if (tray_icon_)
        tray_icon_->setToolTip(currentTooltip());
}

void TrayPresence::applyIcon() {
    if (!tray_icon_)
        return;

    static const QString kIdlePath = QStringLiteral(":/brand/exosnap-logo-idle.ico");
    static const QString kRecordingPath = QStringLiteral(":/brand/exosnap-logo-recording.ico");
    static const QString kPausedPath = QStringLiteral(":/brand/exosnap-logo-paused.ico");

    const QString& icon_path = [this]() -> const QString& {
        switch (state_) {
        case TrayIconState::Recording:
            return kRecordingPath;
        case TrayIconState::Paused:
            return kPausedPath;
        case TrayIconState::Idle:
        default:
            return kIdlePath;
        }
    }();

    QIcon icon(icon_path);
    if (icon.isNull()) {
        // Fall back: keep existing icon rather than blanking the tray.
        return;
    }
    tray_icon_->setIcon(icon);
}

// ---------------------------------------------------------------------------
// Unread notification badge (NOTIFY-SKIN-R1)
// ---------------------------------------------------------------------------

void TrayPresence::incrementUnreadCount() {
    ++unread_count_;
    rebuildNotificationsLabel();
}

void TrayPresence::clearUnreadCount() {
    if (unread_count_ == 0)
        return;
    unread_count_ = 0;
    rebuildNotificationsLabel();
}

void TrayPresence::rebuildNotificationsLabel() {
    if (!notifications_action_)
        return;

    if (unread_count_ <= 0) {
        notifications_action_->setVisible(false);
        return;
    }

    notifications_action_->setText(QStringLiteral("Notifications (%1)").arg(unread_count_));
    notifications_action_->setVisible(true);
}

void TrayPresence::onTrayActivated(QSystemTrayIcon::ActivationReason reason) {
    // TRAY-CLOSE-TO-TRAY-R1 click semantics (from Mappe "Tray behavior" SpecBox):
    //   Left-click (Trigger)  → show / focus window
    //   Double-click          → start / stop recording
    //   Right-click           → context menu (handled by Qt via setContextMenu)
    if (reason == QSystemTrayIcon::Trigger)
        emit activateWindowRequested();
    else if (reason == QSystemTrayIcon::DoubleClick)
        emit recordToggleRequested();
}

void TrayPresence::onShowHideTriggered() {
    emit activateWindowRequested();
}

// ---------------------------------------------------------------------------
// ShouldHideToTray (TRAY-CLOSE-TO-TRAY-R1)
// ---------------------------------------------------------------------------

bool ShouldHideToTray(bool keep_running_in_tray, bool force_quit, bool tray_available) noexcept {
    // Hide to tray only when:
    //   • the user has opted in (keep_running_in_tray == true), AND
    //   • this is not a forced quit (e.g. from tray menu "Quit"), AND
    //   • the system tray is actually available.
    return keep_running_in_tray && !force_quit && tray_available;
}

} // namespace exosnap::ui::tray
