#include "TrayPresence.h"

#include "models/RecordingPulse.h"

#include <QAction>
#include <QIcon>
#include <QMenu>
#include <QSystemTrayIcon>

namespace exosnap::ui::tray {

namespace {

// The pre-rendered heartbeat. Windows has no animated-icon API, so the pulse is
// a swap between these; they are loaded once into a static QIcon cache because
// the swap runs for the whole length of a recording.
//
// The peak frame IS the plain recording mark, so the beat is trough, rise, peak,
// fall over four frames (models/RecordingPulse.h).
const QIcon& RecordingPulseIcon(int frame) {
    static const QIcon frames[kRecordingPulseFrameCount] = {
        QIcon(QStringLiteral(":/brand/exosnap-logo-recording-p0.ico")),
        QIcon(QStringLiteral(":/brand/exosnap-logo-recording-p1.ico")),
        QIcon(QStringLiteral(":/brand/exosnap-logo-recording.ico")),
        QIcon(QStringLiteral(":/brand/exosnap-logo-recording-p1.ico")),
    };
    const int index = (frame >= 0 && frame < kRecordingPulseFrameCount) ? frame : 0;
    return frames[index];
}

const QIcon& StaticIcon(ShellIconState state) {
    static const QIcon idle(QStringLiteral(":/brand/exosnap-logo-idle.ico"));
    static const QIcon paused(QStringLiteral(":/brand/exosnap-logo-paused.ico"));
    static const QIcon saved(QStringLiteral(":/brand/exosnap-logo-saved.ico"));
    static const QIcon recording(QStringLiteral(":/brand/exosnap-logo-recording.ico"));

    switch (state) {
    case ShellIconState::Recording:
        return recording;
    case ShellIconState::Paused:
        return paused;
    case ShellIconState::Saved:
        return saved;
    case ShellIconState::Idle:
        break;
    }
    return idle;
}

QString ActionLabel(ShellAction action) {
    switch (action) {
    case ShellAction::Start:
        return QObject::tr("Start recording");
    case ShellAction::Pause:
        return QObject::tr("Pause recording");
    case ShellAction::Resume:
        return QObject::tr("Resume recording");
    case ShellAction::Stop:
        return QObject::tr("Stop recording");
    case ShellAction::None:
        break;
    }
    return {};
}

// Applies one projection row to one menu entry. An entry whose action is not
// valid in this state is hidden rather than shown-and-failing; the Record entry
// is the documented exception, because a start that is momentarily refused has a
// reason and a vanished entry does not.
void ApplyAppearance(QAction* action, const ShellButtonAppearance& appearance, bool keep_visible_when_disabled) {
    if (action == nullptr)
        return;
    const bool visible = appearance.visible && (appearance.enabled || keep_visible_when_disabled);
    action->setVisible(visible);
    action->setEnabled(appearance.enabled);
    const QString label = ActionLabel(appearance.action);
    if (!label.isEmpty())
        action->setText(label);
}

} // namespace

// ---------------------------------------------------------------------------
// TrayPresence
// ---------------------------------------------------------------------------

TrayPresence::TrayPresence(QObject* parent) : QObject(parent) {
    // tray_icon_ is parented to this (QObject), so it will be destroyed
    // automatically.
    tray_icon_ = new QSystemTrayIcon(this);

    // The menu stays parentless and is deleted by hand in the destructor.
    // setContextMenu() does NOT take ownership -- QSystemTrayIcon holds it as a
    // QPointer -- so giving it a parent here would be the fiction, not the plain
    // delete below.
    tray_menu_ = new QMenu();

    show_hide_action_ = tray_menu_->addAction(tr("Show window"));
    record_action_ = tray_menu_->addAction(ActionLabel(ShellAction::Start));
    pause_resume_action_ = tray_menu_->addAction(ActionLabel(ShellAction::Pause));
    stop_action_ = tray_menu_->addAction(ActionLabel(ShellAction::Stop));
    tray_menu_->addSeparator();
    // NOTIFY-SKIN-R1: clickable mirror for over-game toasts.
    // Clicking this focuses/shows the ExoSnap window (the spec's named mechanism).
    // Label is updated as "Notifications (N)" when N > 0, else hidden.
    notifications_action_ = tray_menu_->addAction(tr("Notifications"));
    notifications_action_->setVisible(false); // hidden until there are unread items
    tray_menu_->addSeparator();
    quit_action_ = tray_menu_->addAction(tr("Quit ExoSnap"));

    connect(show_hide_action_, &QAction::triggered, this, &TrayPresence::onShowHideTriggered);
    connect(record_action_, &QAction::triggered, this, [this]() { requestAction(ShellButton::Record); });
    connect(pause_resume_action_, &QAction::triggered, this, [this]() { requestAction(ShellButton::PauseResume); });
    connect(stop_action_, &QAction::triggered, this, [this]() { requestAction(ShellButton::Stop); });
    // Notifications action: clicking focuses the window and clears the badge.
    connect(notifications_action_, &QAction::triggered, this, [this]() {
        clearUnreadCount();
        emit activateWindowRequested();
    });
    connect(quit_action_, &QAction::triggered, this, &TrayPresence::quitRequested);

    tray_icon_->setContextMenu(tray_menu_);

    connect(tray_icon_, &QSystemTrayIcon::activated, this, &TrayPresence::onTrayActivated);

    applyIcon();
    applyMenuState();
    rebuildTooltip();
}

TrayPresence::~TrayPresence() {
    // tray_menu_ is a QWidget with no parent; delete it explicitly before
    // tray_icon_ (which does not own the menu despite holding a pointer).
    delete tray_menu_;
    tray_menu_ = nullptr;
}

void TrayPresence::applyState(const ShellPresenceState& state, const QString& elapsed_text, int pulse_frame) {
    // Every synchronizeRecordState() ends here, and that runs on the
    // diagnostics and metrics cadences as well as on real state changes -- so
    // this was constructing a QIcon from a resource, re-setting the tray icon,
    // rebuilding the tooltip and rewriting menu-item properties several times a
    // second to arrive at exactly what was already there. The adapters upstream
    // are all change-guarded; this leaf was not.
    if (state_applied_ && state_ == state && elapsed_text_ == elapsed_text && pulse_frame_ == pulse_frame)
        return;

    const bool icon_changed = !state_applied_ || state_.icon_state != state.icon_state || pulse_frame_ != pulse_frame;
    const bool menu_changed = !state_applied_ || state_ != state;

    state_applied_ = true;
    state_ = state;
    elapsed_text_ = elapsed_text;
    pulse_frame_ = pulse_frame;

    if (icon_changed)
        applyIcon();
    if (menu_changed)
        applyMenuState();
    rebuildTooltip();
}

void TrayPresence::updateElapsedText(const QString& elapsed_text) {
    if (elapsed_text_ == elapsed_text)
        return;
    elapsed_text_ = elapsed_text;
    rebuildTooltip();
}

void TrayPresence::setWindowVisible(bool visible) {
    window_visible_ = visible;
    if (show_hide_action_ != nullptr)
        show_hide_action_->setText(visible ? tr("Hide window") : tr("Show window"));
}

void TrayPresence::show() {
    if (tray_icon_ != nullptr)
        tray_icon_->show();
}

void TrayPresence::hide() {
    if (tray_icon_ != nullptr)
        tray_icon_->hide();
}

QString TrayPresence::currentTooltip() const {
    // Tooltip format per Mappe "Tray behavior" SpecBox:
    //   "ExoSnap — Ready"
    //   "ExoSnap — Recording 04:17"
    //   "ExoSnap — Paused"
    QString tip = QStringLiteral("ExoSnap — ");

    switch (state_.icon_state) {
    case ShellIconState::Recording:
        tip += tr("Recording");
        if (!elapsed_text_.isEmpty())
            tip += QLatin1Char(' ') + elapsed_text_;
        break;
    case ShellIconState::Paused:
        tip += tr("Paused");
        break;
    case ShellIconState::Saved:
        tip += tr("Saved");
        break;
    case ShellIconState::Idle:
        tip += tr("Ready");
        break;
    }

    return tip;
}

void TrayPresence::rebuildTooltip() {
    if (tray_icon_ != nullptr)
        tray_icon_->setToolTip(currentTooltip());
}

void TrayPresence::applyIcon() {
    if (tray_icon_ == nullptr)
        return;

    const QIcon& icon = state_.icon_state == ShellIconState::Recording ? RecordingPulseIcon(pulse_frame_)
                                                                       : StaticIcon(state_.icon_state);
    if (icon.isNull()) {
        // Keep the existing icon rather than blanking the tray.
        return;
    }
    tray_icon_->setIcon(icon);
}

void TrayPresence::applyMenuState() {
    // Record stays visible while it is refused; the other two disappear, because
    // there is genuinely nothing to pause or stop.
    ApplyAppearance(record_action_, ShellButtonFor(ShellButton::Record, state_), /*keep_visible_when_disabled=*/true);
    ApplyAppearance(pause_resume_action_, ShellButtonFor(ShellButton::PauseResume, state_), false);
    ApplyAppearance(stop_action_, ShellButtonFor(ShellButton::Stop, state_), false);
}

void TrayPresence::requestAction(ShellButton button) {
    const ShellButtonAppearance appearance = ShellButtonFor(button, state_);
    if (!appearance.visible || !appearance.enabled || appearance.action == ShellAction::None)
        return;
    emit shellActionRequested(appearance.action);
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
    if (notifications_action_ == nullptr)
        return;

    if (unread_count_ <= 0) {
        notifications_action_->setVisible(false);
        return;
    }

    notifications_action_->setText(tr("Notifications (%1)").arg(unread_count_));
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
    // The label decides, from the same flag that wrote it in setWindowVisible().
    if (window_visible_) {
        emit hideWindowRequested();
        return;
    }
    emit activateWindowRequested();
}

} // namespace exosnap::ui::tray
