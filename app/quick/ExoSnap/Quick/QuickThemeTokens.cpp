#include "QuickThemeTokens.h"

#include "ui/theme/ExoSnapThemes.h"

#include <QGuiApplication>
#include <QPalette>
#include <QRegularExpression>
#include <QVariantMap>

#include <algorithm>

namespace exosnap::quick {
namespace {

using ui::theme::ExoAccent;
using ui::theme::ExoAppearance;
using ui::theme::kDefaultAccentId;
using ui::theme::kDefaultAppearanceId;
using ui::theme::kExoAccents;
using ui::theme::kExoAppearances;
using ui::theme::kExoThemeMigrations;
using ui::theme::ThemeKind;

// The tables store line tokens as CSS `rgba(r, g, b, a)` strings, which QColor
// does not parse; plain hex values pass straight through.
QColor parseToken(const char* token) {
    if (token == nullptr) {
        return {};
    }
    const QString value = QString::fromUtf8(token).trimmed();
    if (!value.startsWith(QLatin1String("rgba("))) {
        return QColor(value);
    }
    static const QRegularExpression pattern(
        QStringLiteral("^rgba\\(\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*([0-9.]+)\\s*\\)$"));
    const QRegularExpressionMatch match = pattern.match(value);
    if (!match.hasMatch()) {
        return {};
    }
    QColor color(match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt());
    color.setAlphaF(static_cast<float>(std::clamp(match.captured(4).toDouble(), 0.0, 1.0)));
    return color;
}

QColor withAlpha(QColor color, double alpha) {
    color.setAlphaF(static_cast<float>(std::clamp(alpha, 0.0, 1.0)));
    return color;
}

QColor blend(const QColor& from, const QColor& to, double t) {
    const double s = std::clamp(t, 0.0, 1.0);
    return QColor(qRound(from.red() * (1.0 - s) + to.red() * s), qRound(from.green() * (1.0 - s) + to.green() * s),
                  qRound(from.blue() * (1.0 - s) + to.blue() * s));
}

// The Dark appearance, which is what every fixed-dark surface resolves against
// regardless of the application appearance. Its position in the table is not
// assumed: it is found by kind.
const ExoAppearance& darkAppearance() {
    for (const ExoAppearance& appearance : kExoAppearances) {
        if (appearance.kind == ThemeKind::Dark) {
            return appearance;
        }
    }
    return kExoAppearances.front();
}

const ExoAppearance& resolveAppearance(const QString& appearance_id) {
    for (const ExoAppearance& appearance : kExoAppearances) {
        if (appearance_id == QLatin1StringView(appearance.id)) {
            return appearance;
        }
    }
    return kExoAppearances.front();
}

const ExoAccent& resolveAccent(const QString& accent_id) {
    for (const ExoAccent& accent : kExoAccents) {
        if (accent_id == QLatin1StringView(accent.id)) {
            return accent;
        }
    }
    return kExoAccents.front();
}

} // namespace

QuickThemeTokens::QuickThemeTokens(QObject* parent) : QObject(parent) {
    setAppearance(QString::fromUtf8(kDefaultAppearanceId), QString::fromUtf8(kDefaultAccentId));
}

void QuickThemeTokens::setAppearance(const QString& appearance_id, const QString& accent_id) {
    const ExoAppearance& appearance = resolveAppearance(appearance_id);
    const ExoAccent& accent = resolveAccent(accent_id);
    const QString resolved_appearance = QString::fromUtf8(appearance.id);
    const QString resolved_accent = QString::fromUtf8(accent.id);
    if (appearance_id_ == resolved_appearance && accent_id_ == resolved_accent) {
        return;
    }

    appearance_id_ = resolved_appearance;
    accent_id_ = resolved_accent;
    dark_ = appearance.kind == ThemeKind::Dark;

    background_ = parseToken(appearance.bg);
    surface_ = parseToken(appearance.surf);
    surface_raised_ = parseToken(appearance.surf2);
    surface_hover_ = parseToken(appearance.raise);
    line_ = parseToken(appearance.line);
    line_strong_ = parseToken(appearance.line2);
    text_ = parseToken(appearance.ink);
    text_secondary_ = parseToken(appearance.text1);
    text_muted_ = parseToken(appearance.mut);
    text_dim_ = parseToken(appearance.dim);
    warning_ = parseToken(appearance.caution);
    error_ = parseToken(appearance.error);
    error_ink_ = parseToken(appearance.error_ink);
    success_ = parseToken(appearance.success);
    success_text_ = parseToken(appearance.success_text);
    warning_text_ = parseToken(appearance.caution_text);
    error_text_ = parseToken(appearance.error_text);

    accent_ = parseToken(dark_ ? accent.dark : accent.light);
    accent_ink_ = parseToken(dark_ ? accent.dark_ink : accent.light_ink);
    overlay_accent_ = parseToken(accent.dark);

    // Applied here rather than built here: the palette is a pure function of the
    // resolved tokens, which is what lets a test check it without an application
    // object to hang it on.
    if (QGuiApplication::instance() != nullptr)
        QGuiApplication::setPalette(widgetsPalette());

    // Ink for content on a success- or caution-FILLED surface, completing the
    // set the table already curates for the two fills whose hue moves: the
    // accent (`accent_ink`, per accent AND per appearance) and error
    // (`error_ink`, white in Light, a warm near-black in Dark). Success and
    // caution are light fills in BOTH appearances, so one near-black reads on
    // either; it is warmed toward its own fill the way the authored
    // `error_ink` is, rather than being a second literal black.
    success_ink_ = blend(QColor(0x0B, 0x0B, 0x0C), success_, 0.08);
    warning_ink_ = blend(QColor(0x0B, 0x0B, 0x0C), warning_, 0.08);

    // Tinted notice grounds have no entry in the shared tables (they are a
    // design-system derivation, not a palette value). Derived from this
    // appearance's own base so both appearances stay consistent instead of
    // hardcoding one palette's values.
    //
    // From `surface`, not from `background`: a notice is CONTENT and sits on a
    // card with the rest of it. Tinting the page ground instead tied the notice
    // to whatever the frame was doing -- when Light's ground was darkened to stop
    // the UI glaring, the notice went with it and its three text rungs fell from
    // 4.5:1 to 4.2:1 on a surface no page colour should have reached.
    //
    // Light tints harder than it used to (0.20 rather than 0.16) because it is
    // now tinting a lighter base. The strength is not a taste setting: the
    // indicator rung has to stay UNREADABLE on its own tinted ground, which is
    // the whole reason `warningText` and its two peers exist, and a ground too
    // close to the card stops making that case.
    warning_surface_ = blend(surface_, warning_, dark_ ? 0.13 : 0.20);
    error_surface_ = blend(surface_, error_, dark_ ? 0.13 : 0.20);

    // The modal scrim. A semantic role, not a translucent copy of the current
    // page background: it means "the application behind this is not the thing
    // to act on right now". Deriving it from `background` (which is what the
    // overlay card used to do) works in Dark by coincidence — the background IS
    // dark — and fails in Light, where washing the shell toward white leaves a
    // white card floating on a white page with only its border separating them.
    //
    // So both appearances darken, and only the strength differs: Dark keeps the
    // accepted 0.78 over its own near-black ground, Light uses a restrained
    // neutral ink at 0.38 — enough for the shell to recede and the card to sit
    // clearly in front, far short of the black curtain a heavier value makes of
    // a light page.
    overlay_scrim_ = dark_ ? withAlpha(background_, 0.78) : withAlpha(QColor(0x0F, 0x0F, 0x11), 0.38);

    emit changed();
}

QVariantList QuickThemeTokens::appearanceOptions() {
    QVariantList options;
    for (const ExoAppearance& appearance : kExoAppearances) {
        QVariantMap entry;
        entry.insert(QStringLiteral("value"), QString::fromUtf8(appearance.id));
        entry.insert(QStringLiteral("label"), QString::fromUtf8(appearance.name));
        entry.insert(QStringLiteral("selectable"), true);
        entry.insert(QStringLiteral("reason"), QString::fromUtf8(appearance.intent));
        options.append(entry);
    }
    return options;
}

QVariantList QuickThemeTokens::accentOptions(const QString& appearance_id) {
    const bool dark = resolveAppearance(appearance_id).kind == ThemeKind::Dark;
    QVariantList options;
    for (const ExoAccent& accent : kExoAccents) {
        QVariantMap entry;
        entry.insert(QStringLiteral("value"), QString::fromUtf8(accent.id));
        entry.insert(QStringLiteral("label"), QString::fromUtf8(accent.name));
        entry.insert(QStringLiteral("selectable"), true);
        entry.insert(QStringLiteral("reason"), QString::fromUtf8(accent.intent));
        entry.insert(QStringLiteral("swatch"), QString::fromUtf8(dark ? accent.dark : accent.light));
        options.append(entry);
    }
    return options;
}

QString QuickThemeTokens::migratedAppearanceId(const QString& legacy_theme_id) {
    return QString::fromUtf8(ui::theme::MigratedAppearanceId(legacy_theme_id.toStdString()));
}

QString QuickThemeTokens::migratedAccentId(const QString& legacy_theme_id) {
    return QString::fromUtf8(ui::theme::MigratedAccentId(legacy_theme_id.toStdString()));
}

const QString& QuickThemeTokens::appearanceId() const noexcept {
    return appearance_id_;
}
const QString& QuickThemeTokens::accentId() const noexcept {
    return accent_id_;
}
bool QuickThemeTokens::dark() const noexcept {
    return dark_;
}
QColor QuickThemeTokens::background() const noexcept {
    return background_;
}
QColor QuickThemeTokens::surface() const noexcept {
    return surface_;
}
QColor QuickThemeTokens::surfaceRaised() const noexcept {
    return surface_raised_;
}
QColor QuickThemeTokens::surfaceHover() const noexcept {
    return surface_hover_;
}
QColor QuickThemeTokens::line() const noexcept {
    return line_;
}
QColor QuickThemeTokens::lineStrong() const noexcept {
    return line_strong_;
}
QColor QuickThemeTokens::text() const noexcept {
    return text_;
}
QColor QuickThemeTokens::textSecondary() const noexcept {
    return text_secondary_;
}
QColor QuickThemeTokens::textMuted() const noexcept {
    return text_muted_;
}
QColor QuickThemeTokens::textDim() const noexcept {
    return text_dim_;
}
QColor QuickThemeTokens::accent() const noexcept {
    return accent_;
}
QColor QuickThemeTokens::accentInk() const noexcept {
    return accent_ink_;
}
QColor QuickThemeTokens::warning() const noexcept {
    return warning_;
}
QColor QuickThemeTokens::warningSurface() const noexcept {
    return warning_surface_;
}
QColor QuickThemeTokens::error() const noexcept {
    return error_;
}
QColor QuickThemeTokens::errorInk() const noexcept {
    return error_ink_;
}
QColor QuickThemeTokens::errorSurface() const noexcept {
    return error_surface_;
}
QColor QuickThemeTokens::success() const noexcept {
    return success_;
}
QColor QuickThemeTokens::successInk() const noexcept {
    return success_ink_;
}
QColor QuickThemeTokens::warningInk() const noexcept {
    return warning_ink_;
}
QColor QuickThemeTokens::successText() const noexcept {
    return success_text_;
}
QColor QuickThemeTokens::warningText() const noexcept {
    return warning_text_;
}
QColor QuickThemeTokens::errorText() const noexcept {
    return error_text_;
}
QColor QuickThemeTokens::overlayScrim() const noexcept {
    return overlay_scrim_;
}
QColor QuickThemeTokens::overlayInk() noexcept {
    return parseToken(darkAppearance().ink);
}
QColor QuickThemeTokens::overlayInkSecondary() noexcept {
    return parseToken(darkAppearance().text1);
}
QColor QuickThemeTokens::overlayInkMuted() noexcept {
    return parseToken(darkAppearance().mut);
}
QColor QuickThemeTokens::overlaySurface() noexcept {
    return parseToken(darkAppearance().surf);
}
QColor QuickThemeTokens::overlaySurfaceRaised() noexcept {
    return parseToken(darkAppearance().surf2);
}
QColor QuickThemeTokens::overlayLine() noexcept {
    return parseToken(darkAppearance().line);
}
QColor QuickThemeTokens::overlayLineStrong() noexcept {
    return parseToken(darkAppearance().line2);
}
QColor QuickThemeTokens::overlayInkDim() noexcept {
    return parseToken(darkAppearance().dim);
}
QColor QuickThemeTokens::overlaySuccess() noexcept {
    return parseToken(darkAppearance().success);
}
QColor QuickThemeTokens::overlayWarning() noexcept {
    return parseToken(darkAppearance().caution);
}
QColor QuickThemeTokens::overlayError() noexcept {
    return parseToken(darkAppearance().error);
}
QColor QuickThemeTokens::overlayAccent() const noexcept {
    return overlay_accent_;
}

QPalette QuickThemeTokens::widgetsPalette() const {
    QPalette palette = QGuiApplication::instance() != nullptr ? QGuiApplication::palette() : QPalette();

    // Menu ground and ink. `surfaceRaised` rather than `background`: a menu is a
    // surface that floats above the window, and the same rung the application's
    // own popovers sit on is what makes it look like part of this product.
    palette.setColor(QPalette::Window, surface_raised_);
    palette.setColor(QPalette::WindowText, text_);
    palette.setColor(QPalette::Base, surface_raised_);
    palette.setColor(QPalette::Text, text_);
    palette.setColor(QPalette::Button, surface_raised_);
    palette.setColor(QPalette::ButtonText, text_);
    palette.setColor(QPalette::ToolTipBase, surface_raised_);
    palette.setColor(QPalette::ToolTipText, text_);

    // The hovered row. Accent and its own ink, the same pair every primary
    // control in the application uses, so a highlighted menu row cannot be a
    // different colour from the control that opened it.
    palette.setColor(QPalette::Highlight, accent_);
    palette.setColor(QPalette::HighlightedText, accent_ink_);

    // A refused row. `dim` is the rung the product already draws an unavailable
    // control's icon in, and it clears the 3:1 the contrast gate holds a
    // non-text element to on this surface.
    palette.setColor(QPalette::Disabled, QPalette::WindowText, text_dim_);
    palette.setColor(QPalette::Disabled, QPalette::Text, text_dim_);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, text_dim_);

    palette.setColor(QPalette::Link, accent_);
    return palette;
}

} // namespace exosnap::quick
