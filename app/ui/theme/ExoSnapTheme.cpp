#include "ExoSnapTheme.h"

#include <algorithm>

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QPalette>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QVector>
#include <QtGlobal>

#include "ExoSnapAccents.h"
#include "ExoSnapMetrics.h"
#include "ExoSnapPalette.h"

namespace exosnap::ui::theme {
namespace {

// ---------------------------------------------------------------------------
// Accent color derivation
// ---------------------------------------------------------------------------

// Resolve an accent entry by id; returns nullptr if not found.
const ExoSnapAccent* FindAccent(const QString& accent_id) {
    for (const auto& a : kExoSnapAccents) {
        if (QString::fromUtf8(a.id) == accent_id)
            return &a;
    }
    return nullptr;
}

// The default accent entry (first in the curated table = Studio Mint).
const ExoSnapAccent& DefaultAccent() {
    return kExoSnapAccents.front();
}

// Resolve an accent by id, falling back to the default accent when not found.
const ExoSnapAccent& ResolveAccent(const QString& accent_id) {
    if (const ExoSnapAccent* a = FindAccent(accent_id))
        return *a;
    qWarning().noquote() << "ExoSnap: unknown accent id" << accent_id << "— falling back to default.";
    return DefaultAccent();
}

// Derived alpha token: "rgba(R, G, B, alpha)" from a hex base color.
QString RgbaToken(const QColor& base, double alpha) {
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(base.red())
        .arg(base.green())
        .arg(base.blue())
        .arg(QString::number(alpha, 'f', 2));
}

// Lighten a color by blending toward white by `factor` (0=unchanged, 1=white).
QColor Lighten(const QColor& c, double factor) {
    const double f = std::clamp(factor, 0.0, 1.0);
    return QColor(qRound(c.red() + (255 - c.red()) * f), qRound(c.green() + (255 - c.green()) * f),
                  qRound(c.blue() + (255 - c.blue()) * f));
}

// Darken a color by blending toward black by `factor` (0=unchanged, 1=black).
QColor Darken(const QColor& c, double factor) {
    const double f = std::clamp(factor, 0.0, 1.0);
    return QColor(qRound(c.red() * (1.0 - f)), qRound(c.green() * (1.0 - f)), qRound(c.blue() * (1.0 - f)));
}

struct AccentTokens {
    QString accent;         // ${accent}
    QString accent_ink;     // ${accent-ink}
    QString accent_dim;     // ${accent-dim}    alpha 0.14
    QString accent_soft;    // ${accent-soft}   alpha 0.24
    QString accent_line;    // ${accent-line}   alpha 0.42
    QString accent_b2;      // ${accent-b2}     alpha 0.60
    QString accent_hover;   // ${accent-hover}  slightly lighter
    QString accent_pressed; // ${accent-pressed} slightly darker
};

AccentTokens DeriveAccentTokens(const ExoSnapAccent& accent) {
    const QColor base(QString::fromUtf8(accent.base));
    AccentTokens t;
    t.accent = QString::fromUtf8(accent.base);
    t.accent_ink = QString::fromUtf8(accent.ink);
    t.accent_dim = RgbaToken(base, 0.14);
    t.accent_soft = RgbaToken(base, 0.24);
    t.accent_line = RgbaToken(base, 0.42);
    t.accent_b2 = RgbaToken(base, 0.60);
    t.accent_hover = Lighten(base, 0.14).name();
    t.accent_pressed = Darken(base, 0.09).name();
    return t;
}

// Verify that the default accent derives the same values as the static palette
// constants, so we can trust DeriveAccentTokens is consistent.
// (Checked in debug builds only; tolerance for rounding in alpha strings.)

constexpr bool CStrEqual(const char* a, const char* b) {
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

// Keep the curated accent table and the active palette in sync at compile time.
static_assert(CStrEqual(kDefaultAccentId, kExoSnapAccents.front().id),
              "kDefaultAccentId must reference the first curated accent.");
static_assert(CStrEqual(kExoSnapAccents.front().base, ExoSnapPalette::kAccent),
              "Default curated accent base must match ExoSnapPalette::kAccent.");
static_assert(CStrEqual(kExoSnapAccents.front().ink, ExoSnapPalette::kAccentInk),
              "Default curated accent ink must match ExoSnapPalette::kAccentInk.");

struct ThemeToken {
    QString key;
    QString value;
};

struct ThemeFontFamilies {
    // D2: Segoe UI is the blessed system face for UI text (no bundled sans).
    // IBM Plex Mono is bundled (Regular + Medium) for the mandatory mono voice.
    QString sans_primary = QStringLiteral("Segoe UI");
    QString mono_primary = QStringLiteral("IBM Plex Mono");
};

bool ContainsFamily(const QStringList& families, const QString& family) {
    for (const QString& existing : families) {
        if (existing.compare(family, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

void AppendUniqueFamily(QStringList& families, const QString& family) {
    if (family.isEmpty() || ContainsFamily(families, family))
        return;
    families.append(family);
}

QString SelectPreferredFamily(const QStringList& discovered_families, const QString& fallback_family) {
    for (const QString& family : discovered_families) {
        if (family.compare(fallback_family, Qt::CaseInsensitive) == 0)
            return family;
    }

    if (!discovered_families.isEmpty())
        return discovered_families.first();

    return fallback_family;
}

QStringList BuildFamilyStack(const QString& primary_family, const QStringList& fallback_families) {
    QStringList families;
    AppendUniqueFamily(families, primary_family);
    for (const QString& fallback : fallback_families)
        AppendUniqueFamily(families, fallback);
    return families;
}

QStringList SansFamilies(const ThemeFontFamilies& font_families) {
    // D2: Segoe UI Variable Text / Segoe UI are the blessed 1.0 UI faces (system fonts;
    // nothing bundled for sans). Hanken Grotesk is deferred to 1.1.
    return BuildFamilyStack(QStringLiteral("Segoe UI Variable Text"),
                            {QStringLiteral("Segoe UI"), font_families.sans_primary, QStringLiteral("system-ui"),
                             QStringLiteral("sans-serif")});
}

QStringList MonoFamilies(const ThemeFontFamilies& font_families) {
    // D2: IBM Plex Mono is the mandatory 1.0 mono voice — bundled as Regular + Medium TTFs
    // via QFontDatabase. Consolas is the platform fallback only; JetBrains Mono is removed.
    return BuildFamilyStack(QStringLiteral("IBM Plex Mono"),
                            {font_families.mono_primary, QStringLiteral("Consolas"), QStringLiteral("monospace")});
}

QString CssFontFamily(const QStringList& families) {
    QStringList escaped;
    escaped.reserve(families.size());
    for (const QString& family : families) {
        if (family.contains(' ') || family.contains('-'))
            escaped.append(QStringLiteral("\"%1\"").arg(family));
        else
            escaped.append(family);
    }
    return escaped.join(", ");
}

QString ReadThemeQss() {
    QFile file(":/theme/exosnap_dark.qss");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning("Failed to load ExoSnap theme QSS resource.");
        return {};
    }

    QTextStream stream(&file);
    return stream.readAll();
}

QVector<ThemeToken> BuildTokens(const ThemeFontFamilies& font_families, const AccentTokens& at) {
    return {
        {"${bg0}", ExoSnapPalette::kBg0},
        {"${bg1}", ExoSnapPalette::kBg1},
        {"${bg2}", ExoSnapPalette::kBg2},
        {"${bg3}", ExoSnapPalette::kBg3},
        {"${bg4}", ExoSnapPalette::kBg4},
        {"${line1}", ExoSnapPalette::kLine1},
        {"${line2}", ExoSnapPalette::kLine2},
        {"${line3}", ExoSnapPalette::kLine3},
        {"${text0}", ExoSnapPalette::kText0},
        {"${text1}", ExoSnapPalette::kText1},
        {"${text2}", ExoSnapPalette::kText2},
        {"${text3}", ExoSnapPalette::kText3},
        {"${accent}", at.accent},
        {"${accent-ink}", at.accent_ink},
        {"${accent-dim}", at.accent_dim},
        {"${accent-soft}", at.accent_soft},
        {"${accent-line}", at.accent_line},
        {"${accent-b2}", at.accent_b2},
        {"${accent-hover}", at.accent_hover},
        {"${accent-pressed}", at.accent_pressed},
        {"${ok}", ExoSnapPalette::kOk},
        {"${warn}", ExoSnapPalette::kWarn},
        {"${err}", ExoSnapPalette::kErr},
        {"${info}", ExoSnapPalette::kInfo},
        {"${space-xs}", QString::number(ExoSnapMetrics::kSpaceXs)},
        {"${space-sm}", QString::number(ExoSnapMetrics::kSpaceSm)},
        {"${space-md}", QString::number(ExoSnapMetrics::kSpaceMd)},
        {"${space-lg}", QString::number(ExoSnapMetrics::kSpaceLg)},
        {"${space-xl}", QString::number(ExoSnapMetrics::kSpaceXl)},
        {"${space-2xl}", QString::number(ExoSnapMetrics::kSpace2xl)},
        {"${radius-sm}", QString::number(ExoSnapMetrics::kRadiusSm)},
        {"${radius-md}", QString::number(ExoSnapMetrics::kRadiusMd)},
        {"${radius-lg}", QString::number(ExoSnapMetrics::kRadiusLg)},
        {"${control-height}", QString::number(ExoSnapMetrics::kControlHeight)},
        {"${primary-cta-height}", QString::number(ExoSnapMetrics::kPrimaryCtaHeight)},
        {"${font-sans}", CssFontFamily(SansFamilies(font_families))},
        {"${font-mono}", CssFontFamily(MonoFamilies(font_families))},
    };
}

QStringList FindUnresolvedThemeTokens(const QString& stylesheet) {
    static const QRegularExpression kTokenPattern(R"(\$\{([^}]+)\})");
    QStringList unresolved;

    QRegularExpressionMatchIterator it = kTokenPattern.globalMatch(stylesheet);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        unresolved.append(match.captured(1));
    }

    unresolved.removeDuplicates();
    unresolved.sort();
    return unresolved;
}

QString BuildThemeStylesheet(const ThemeFontFamilies& font_families, const AccentTokens& at) {
    QString qss = ReadThemeQss();
    if (qss.isEmpty())
        return qss;

    for (const auto& token : BuildTokens(font_families, at))
        qss.replace(token.key, token.value);

    const QStringList unresolved = FindUnresolvedThemeTokens(qss);
    if (!unresolved.isEmpty()) {
        qWarning().noquote() << "ExoSnap theme token replacement incomplete. Unresolved tokens:"
                             << unresolved.join(", ");
#if defined(QT_DEBUG)
        Q_ASSERT_X(unresolved.isEmpty(), "BuildThemeStylesheet",
                   "Unresolved theme tokens remain in stylesheet. See warning log for token names.");
#endif
    }

    return qss;
}

// Apply the accent-dependent palette roles (highlight + link) for the active accent.
void ApplyAccentPalette(QApplication& app, const AccentTokens& at) {
    QPalette palette = app.palette();
    palette.setColor(QPalette::Highlight, QColor(at.accent));
    palette.setColor(QPalette::HighlightedText, QColor(at.accent_ink));
    palette.setColor(QPalette::Link, QColor(at.accent));
    app.setPalette(palette);
}

// Cached font families after initial load; populated by ApplyExoSnapTheme().
ThemeFontFamilies& CachedFontFamilies() {
    static ThemeFontFamilies font_families;
    return font_families;
}

void ApplyPalette(QApplication& app, const AccentTokens& at) {
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(ExoSnapPalette::kBg0));
    palette.setColor(QPalette::WindowText, QColor(ExoSnapPalette::kText0));
    palette.setColor(QPalette::Base, QColor(ExoSnapPalette::kBg2));
    palette.setColor(QPalette::AlternateBase, QColor(ExoSnapPalette::kBg3));
    palette.setColor(QPalette::Text, QColor(ExoSnapPalette::kText0));
    palette.setColor(QPalette::Button, QColor(ExoSnapPalette::kBg3));
    palette.setColor(QPalette::ButtonText, QColor(ExoSnapPalette::kText1));
    palette.setColor(QPalette::Highlight, QColor(at.accent));
    palette.setColor(QPalette::HighlightedText, QColor(at.accent_ink));
    palette.setColor(QPalette::Link, QColor(at.accent));
    palette.setColor(QPalette::ToolTipBase, QColor(ExoSnapPalette::kBg2));
    palette.setColor(QPalette::ToolTipText, QColor(ExoSnapPalette::kText0));
    palette.setColor(QPalette::PlaceholderText, QColor(ExoSnapPalette::kText2));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(ExoSnapPalette::kText3));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(ExoSnapPalette::kText3));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(ExoSnapPalette::kText3));
    app.setPalette(palette);
}

QStringList LoadFontFamiliesFromResources(const QStringList& font_resources, const QString& label) {
    QStringList discovered_families;
    for (const QString& path : font_resources) {
        if (!QFile::exists(path)) {
            qWarning().noquote() << "ExoSnap font resource missing:" << path;
            continue;
        }

        const int font_id = QFontDatabase::addApplicationFont(path);
        if (font_id < 0) {
            qWarning().noquote() << "Failed to load ExoSnap font resource:" << path;
            continue;
        }

        const QStringList families = QFontDatabase::applicationFontFamilies(font_id);
        if (families.isEmpty()) {
            qWarning().noquote() << "Loaded ExoSnap font resource without family metadata:" << path;
            continue;
        }

        for (const QString& family : families)
            AppendUniqueFamily(discovered_families, family);
    }

    if (discovered_families.isEmpty())
        qWarning().noquote() << "No usable bundled font family discovered for" << label << "fonts.";

    return discovered_families;
}

ThemeFontFamilies LoadBundledFonts() {
    ThemeFontFamilies font_families;

    // D2: IBM Plex Mono (Regular + Medium) is the only bundled family. Segoe UI is a
    // system font; no sans family is loaded from resources.
    const QStringList plex_families = LoadFontFamiliesFromResources(
        {QStringLiteral(":/fonts/IBMPlexMono-Regular.ttf"), QStringLiteral(":/fonts/IBMPlexMono-Medium.ttf")},
        QStringLiteral("IBM Plex Mono"));

    font_families.mono_primary = SelectPreferredFamily(plex_families, font_families.mono_primary);
    return font_families;
}

void ApplyDefaultAppFont(QApplication& app, const ThemeFontFamilies& font_families) {
    QFont app_font;
    app_font.setFamilies(SansFamilies(font_families));
    app_font.setPointSize(10);
    app.setFont(app_font);
}

} // namespace

void ApplyExoSnapTheme(QApplication& app) {
    const ThemeFontFamilies font_families = LoadBundledFonts();
    CachedFontFamilies() = font_families;

    const AccentTokens at = DeriveAccentTokens(DefaultAccent());

    app.setStyle("Fusion");
    ApplyDefaultAppFont(app, font_families);
    ApplyPalette(app, at);
    app.setStyleSheet(BuildThemeStylesheet(font_families, at));
}

void ReapplyAccent(QApplication& app, const QString& accent_id) {
    const ExoSnapAccent& accent = ResolveAccent(accent_id);
    const AccentTokens at = DeriveAccentTokens(accent);

    ApplyAccentPalette(app, at);
    app.setStyleSheet(BuildThemeStylesheet(CachedFontFamilies(), at));
}

} // namespace exosnap::ui::theme
