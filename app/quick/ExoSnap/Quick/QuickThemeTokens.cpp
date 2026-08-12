#include "QuickThemeTokens.h"

#include "ui/theme/ExoSnapThemes.h"

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

QColor blend(const QColor& from, const QColor& to, double t) {
    const double s = std::clamp(t, 0.0, 1.0);
    return QColor(qRound(from.red() * (1.0 - s) + to.red() * s), qRound(from.green() * (1.0 - s) + to.green() * s),
                  qRound(from.blue() * (1.0 - s) + to.blue() * s));
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

    accent_ = parseToken(dark_ ? accent.dark : accent.light);
    accent_ink_ = parseToken(dark_ ? accent.dark_ink : accent.light_ink);

    // Tinted notice grounds have no entry in the shared tables (they are a
    // design-system derivation, not a palette value). Derived from this
    // appearance's own base so both appearances stay consistent instead of
    // hardcoding one palette's values.
    warning_surface_ = blend(background_, warning_, dark_ ? 0.13 : 0.16);
    error_surface_ = blend(background_, error_, dark_ ? 0.13 : 0.16);

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

} // namespace exosnap::quick
