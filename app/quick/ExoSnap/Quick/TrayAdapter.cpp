#include "TrayAdapter.h"

#include "ui/brand/ShellIconRenderer.h"

namespace exosnap::quick {

namespace {

using ui::brand::ShellGlyph;
using ui::brand::ShellGlyphRequest;
using ui::brand::ShellMarkRequest;

[[nodiscard]] QString ActionLabel(ShellAction action) {
    switch (action) {
    case ShellAction::Start:
        return TrayAdapter::tr("Start recording");
    case ShellAction::Pause:
        return TrayAdapter::tr("Pause recording");
    case ShellAction::Resume:
        return TrayAdapter::tr("Resume recording");
    case ShellAction::Stop:
        return TrayAdapter::tr("Stop recording");
    case ShellAction::OpenOutputFolder:
        return TrayAdapter::tr("Open output folder");
    case ShellAction::None:
        break;
    }
    return {};
}

[[nodiscard]] bool GlyphForAction(ShellAction action, ShellGlyph& out) {
    switch (action) {
    case ShellAction::Start:
        out = ShellGlyph::Record;
        return true;
    case ShellAction::Pause:
        out = ShellGlyph::Pause;
        return true;
    case ShellAction::Resume:
        out = ShellGlyph::Resume;
        return true;
    case ShellAction::Stop:
        out = ShellGlyph::Stop;
        return true;
    case ShellAction::OpenOutputFolder:
        out = ShellGlyph::Folder;
        return true;
    case ShellAction::None:
        break;
    }
    return false;
}

[[nodiscard]] ShellButton ButtonFor(TrayAdapter::TransportRow row) {
    switch (row) {
    case TrayAdapter::PauseResumeRow:
        return ShellButton::PauseResume;
    case TrayAdapter::StopRow:
        return ShellButton::Stop;
    case TrayAdapter::RecordRow:
        break;
    }
    return ShellButton::Record;
}

} // namespace

TrayAdapter::TrayAdapter(QObject* parent) : QObject(parent) {
}

void TrayAdapter::setActive(bool active) {
    if (active_ == active)
        return;
    active_ = active;
    emit activeChanged();
}

void TrayAdapter::setPresence(const ShellPresenceState& state, const QString& elapsed_text, int mark_frame) {
    if (state_ == state && elapsed_text_ == elapsed_text && mark_frame_ == mark_frame)
        return;
    state_ = state;
    elapsed_text_ = elapsed_text;
    mark_frame_ = mark_frame;
    emit appearanceChanged();
}

void TrayAdapter::setElapsedText(const QString& elapsed_text) {
    if (elapsed_text_ == elapsed_text)
        return;
    elapsed_text_ = elapsed_text;
    emit appearanceChanged();
}

void TrayAdapter::setBlockedReason(const QString& reason) {
    if (blocked_reason_ == reason)
        return;
    blocked_reason_ = reason;
    emit appearanceChanged();
}

void TrayAdapter::setAppearance(const QString& appearance_id, const QString& accent_id) {
    if (appearance_id_ == appearance_id && accent_id_ == accent_id)
        return;
    appearance_id_ = appearance_id;
    accent_id_ = accent_id;
    emit appearanceChanged();
}

void TrayAdapter::setIconPixelSize(int px) {
    if (px <= 0 || icon_px_ == px)
        return;
    icon_px_ = px;
    emit appearanceChanged();
}

void TrayAdapter::incrementUnreadCount() {
    ++unread_count_;
    emit unreadCountChanged();
}

void TrayAdapter::clearUnreadCount() {
    if (unread_count_ == 0)
        return;
    unread_count_ = 0;
    emit unreadCountChanged();
}

bool TrayAdapter::active() const noexcept {
    return active_;
}

QString TrayAdapter::iconSource() const {
    ShellMarkRequest request;
    request.kind = ui::brand::BrandMarkKindFor(state_.icon_state);
    request.px = icon_px_;
    request.frame = mark_frame_;
    request.appearance_id = appearance_id_;
    request.accent_id = accent_id_;
    return ui::brand::ShellIconImageUrl(ui::brand::MarkImageId(request));
}

QString TrayAdapter::statusText() const {
    switch (state_.icon_state) {
    case ShellIconState::Recording: {
        QString text = tr("Recording");
        if (!elapsed_text_.isEmpty())
            text += QLatin1Char(' ') + elapsed_text_;
        return text;
    }
    case ShellIconState::Paused:
        return tr("Paused");
    case ShellIconState::Saved:
        return tr("Saved");
    case ShellIconState::Processing:
        return tr("Finishing recording");
    case ShellIconState::Error:
        return tr("Recording failed");
    case ShellIconState::Idle:
        break;
    }
    return tr("Ready");
}

QString TrayAdapter::tooltip() const {
    // "ExoSnap - Ready" / "ExoSnap - Recording 04:17" / "ExoSnap - Paused". The
    // same phrase the menu's first row shows, so the two cannot drift apart.
    return QStringLiteral("ExoSnap \xE2\x80\x94 ") + statusText();
}

bool TrayAdapter::blockedReasonVisible() const {
    return state_.phase == ShellPhase::Blocked && !blocked_reason_.isEmpty();
}

const QString& TrayAdapter::blockedReason() const noexcept {
    return blocked_reason_;
}

QString TrayAdapter::glyphUrl(ui::brand::ShellGlyph glyph) const {
    ShellGlyphRequest request;
    request.glyph = glyph;
    request.px = icon_px_;
    request.appearance_id = appearance_id_;
    request.accent_id = accent_id_;
    return ui::brand::ShellIconImageUrl(ui::brand::GlyphImageId(request));
}

QString TrayAdapter::showWindowIcon() const {
    return glyphUrl(ui::brand::ShellGlyph::Window);
}

QString TrayAdapter::outputFolderIcon() const {
    return glyphUrl(ui::brand::ShellGlyph::Folder);
}

QString TrayAdapter::notificationsIcon() const {
    return glyphUrl(ui::brand::ShellGlyph::Notifications);
}

QString TrayAdapter::quitIcon() const {
    return glyphUrl(ui::brand::ShellGlyph::Quit);
}

QVariantMap TrayAdapter::rowFor(ShellButton button, ShellAction fallback_action) const {
    const ShellButtonAppearance appearance = ShellButtonFor(button, state_);
    // The menu keeps a fixed shape where the thumbnail strip does not. A strip
    // is read as a row of controls and closes up around a missing one; a menu is
    // read as a list of everything the application can do, and an entry that is
    // absent in one state and present in the next teaches nothing about why.
    // So a row the table hides is drawn under its default name, greyed.
    const bool offered = appearance.visible && appearance.action != ShellAction::None;
    const ShellAction action = offered ? appearance.action : fallback_action;

    QVariantMap row;
    row.insert(QStringLiteral("visible"), true);
    row.insert(QStringLiteral("enabled"), offered && appearance.enabled);
    row.insert(QStringLiteral("text"), ActionLabel(action));

    ShellGlyph glyph{};
    if (GlyphForAction(action, glyph)) {
        row.insert(QStringLiteral("icon"), glyphUrl(glyph));
    } else {
        row.insert(QStringLiteral("icon"), QString());
    }
    return row;
}

QVariantMap TrayAdapter::recordItem() const {
    return rowFor(ShellButton::Record, ShellAction::Start);
}

QVariantMap TrayAdapter::pauseResumeItem() const {
    return rowFor(ShellButton::PauseResume, ShellAction::Pause);
}

QVariantMap TrayAdapter::stopItem() const {
    return rowFor(ShellButton::Stop, ShellAction::Stop);
}

int TrayAdapter::unreadCount() const noexcept {
    return unread_count_;
}

QString TrayAdapter::notificationsText() const {
    if (unread_count_ <= 0)
        return tr("Notifications");
    return tr("Notifications (%1)").arg(unread_count_);
}

void TrayAdapter::triggerTransport(TransportRow row) {
    const ShellButtonAppearance appearance = ShellButtonFor(ButtonFor(row), state_);
    if (!appearance.visible || !appearance.enabled || appearance.action == ShellAction::None)
        return;
    emit shellActionRequested(appearance.action);
}

void TrayAdapter::triggerShowWindow() {
    emit activateWindowRequested();
}

void TrayAdapter::triggerNotifications() {
    clearUnreadCount();
    emit activateWindowRequested();
}

void TrayAdapter::triggerOpenOutputFolder() {
    emit openOutputFolderRequested();
}

void TrayAdapter::triggerQuit() {
    emit quitRequested();
}

void TrayAdapter::handleActivation(int reason) {
    // A left click and a double click both show or focus the window -- Windows
    // delivers the single click first regardless, so a double click shows the
    // window either way. A right click is the context menu, which the platform
    // opens itself. Toggling a recording stays with the hotkey and the menu: a
    // double click is too easy to produce while reaching for the window to be
    // allowed to start or stop one.
    if (reason == TriggerActivation || reason == DoubleClickActivation)
        emit activateWindowRequested();
}

ShellIconState TrayAdapter::currentIconState() const noexcept {
    return state_.icon_state;
}

int TrayAdapter::currentMarkFrame() const noexcept {
    return mark_frame_;
}

} // namespace exosnap::quick
