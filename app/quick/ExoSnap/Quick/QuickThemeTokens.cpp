#include "QuickThemeTokens.h"

#include "ui/theme/ExoSnapThemes.h"

#include <QRegularExpression>
#include <QVariantMap>

#include <algorithm>

namespace exosnap::quick {
namespace {

using ui::theme::ExoTheme;
using ui::theme::kDefaultThemeId;
using ui::theme::kExoThemes;
using ui::theme::ThemeKind;

// The theme table stores line tokens as CSS `rgba(r, g, b, a)` strings for the
// QSS pipeline. QColor does not parse that form, so convert it here; plain hex
// values pass straight through.
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

const ExoTheme& resolveTheme(const QString& theme_id) {
    for (const ExoTheme& theme : kExoThemes) {
        if (theme_id == QLatin1StringView(theme.id)) {
            return theme;
        }
    }
    return kExoThemes.front();
}

} // namespace

QuickThemeTokens::QuickThemeTokens(QObject* parent) : QObject(parent) {
    setThemeId(QString::fromUtf8(kDefaultThemeId));
}

void QuickThemeTokens::setThemeId(const QString& theme_id) {
    const ExoTheme& theme = resolveTheme(theme_id);
    const QString resolved_id = QString::fromUtf8(theme.id);
    if (theme_id_ == resolved_id) {
        return;
    }

    theme_id_ = resolved_id;
    dark_ = theme.kind == ThemeKind::Dark;

    background_ = parseToken(theme.bg);
    surface_ = parseToken(theme.surf);
    surface_raised_ = parseToken(theme.surf2);
    surface_hover_ = parseToken(theme.raise);
    line_ = parseToken(theme.line);
    line_strong_ = parseToken(theme.line2);
    text_ = parseToken(theme.ink);
    text_muted_ = parseToken(theme.mut);
    text_dim_ = parseToken(theme.dim);
    accent_ = parseToken(theme.ac);
    accent_ink_ = parseToken(theme.ac_ink);
    warning_ = parseToken(theme.caution);
    error_ = parseToken(theme.error);
    error_ink_ = parseToken(theme.error_ink);
    success_ = parseToken(theme.success);

    // Same rule as the QSS pipeline: explicit override wins, otherwise derive.
    text_secondary_ =
        theme.text1_override != nullptr ? parseToken(theme.text1_override) : blend(text_, text_muted_, 0.42);

    // Tinted notice backgrounds have no entry in the shared table (QSS composes
    // them with alpha). Deriving them from this theme's own base keeps all four
    // themes consistent instead of hardcoding one palette's values.
    warning_surface_ = blend(background_, warning_, dark_ ? 0.13 : 0.16);
    error_surface_ = blend(background_, error_, dark_ ? 0.13 : 0.16);

    emit changed();
}

QVariantList QuickThemeTokens::themeOptions() {
    QVariantList options;
    for (const ExoTheme& theme : kExoThemes) {
        QVariantMap entry;
        entry.insert(QStringLiteral("value"), QString::fromUtf8(theme.id));
        entry.insert(QStringLiteral("label"), QString::fromUtf8(theme.name));
        entry.insert(QStringLiteral("selectable"), true);
        entry.insert(QStringLiteral("reason"), QString::fromUtf8(theme.intent));
        options.append(entry);
    }
    return options;
}

const QString& QuickThemeTokens::themeId() const noexcept {
    return theme_id_;
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
