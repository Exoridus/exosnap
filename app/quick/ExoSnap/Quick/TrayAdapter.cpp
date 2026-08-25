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

void TrayAdapter::setWindowVisible(bool visible) {
    if (window_visible_ == visible)
        return;
    window_visible_ = visible;
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

QString TrayAdapter::tooltip() const {
    // "ExoSnap - Ready" / "ExoSnap - Recording 04:17" / "ExoSnap - Paused".
    QString tip = QStringLiteral("ExoSnap \xE2\x80\x94 ");

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
    case ShellIconState::Processing:
        tip += tr("Finishing recording");
        break;
    case ShellIconState::Error:
        tip += tr("Recording failed");
        break;
    case ShellIconState::Idle:
        tip += tr("Ready");
        break;
    }
    return tip;
}

QString TrayAdapter::showHideText() const {
    return window_visible_ ? tr("Hide window") : tr("Show window");
}

QString TrayAdapter::glyphUrl(ui::brand::ShellGlyph glyph) const {
    ShellGlyphRequest request;
    request.glyph = glyph;
    request.px = icon_px_;
    request.appearance_id = appearance_id_;
    request.accent_id = accent_id_;
    return ui::brand::ShellIconImageUrl(ui::brand::GlyphImageId(request));
}

QString TrayAdapter::showHideIcon() const {
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

QVariantMap TrayAdapter::rowFor(ShellButton button, bool keep_visible_when_disabled) const {
    const ShellButtonAppearance appearance = ShellButtonFor(button, state_);

    QVariantMap row;
    row.insert(QStringLiteral("visible"), appearance.visible && (appearance.enabled || keep_visible_when_disabled));
    row.insert(QStringLiteral("enabled"), appearance.enabled);
    row.insert(QStringLiteral("text"), ActionLabel(appearance.action));

    ShellGlyph glyph{};
    if (GlyphForAction(appearance.action, glyph)) {
        row.insert(QStringLiteral("icon"), glyphUrl(glyph));
    } else {
        row.insert(QStringLiteral("icon"), QString());
    }
    return row;
}

QVariantMap TrayAdapter::recordItem() const {
    // Record stays visible while it is refused: a start that is momentarily
    // impossible has a reason, and a vanished entry does not.
    return rowFor(ShellButton::Record, /*keep_visible_when_disabled=*/true);
}

QVariantMap TrayAdapter::pauseResumeItem() const {
    return rowFor(ShellButton::PauseResume, false);
}

QVariantMap TrayAdapter::stopItem() const {
    return rowFor(ShellButton::Stop, false);
}

int TrayAdapter::unreadCount() const noexcept {
    return unread_count_;
}

bool TrayAdapter::notificationsVisible() const noexcept {
    return unread_count_ > 0;
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

void TrayAdapter::triggerShowHide() {
    // The label decides, from the same flag that wrote it.
    if (window_visible_) {
        emit hideWindowRequested();
        return;
    }
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
    // Left click shows or focuses the window; a double click toggles recording;
    // a right click is the context menu, which the platform opens itself.
    if (reason == TriggerActivation) {
        emit activateWindowRequested();
        return;
    }
    if (reason == DoubleClickActivation)
        emit recordToggleRequested();
}

ShellIconState TrayAdapter::currentIconState() const noexcept {
    return state_.icon_state;
}

int TrayAdapter::currentMarkFrame() const noexcept {
    return mark_frame_;
}

} // namespace exosnap::quick
