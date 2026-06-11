#include "ExoSnapTheme.h"

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

QVector<ThemeToken> BuildTokens(const ThemeFontFamilies& font_families) {
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
        {"${accent}", ExoSnapPalette::kAccent},
        {"${accent-ink}", ExoSnapPalette::kAccentInk},
        {"${accent-dim}", ExoSnapPalette::kAccentDim},
        {"${accent-soft}", ExoSnapPalette::kAccentSoft},
        {"${accent-line}", ExoSnapPalette::kAccentLine},
        {"${accent-b2}", ExoSnapPalette::kAccentBorderStrong},
        {"${accent-hover}", ExoSnapPalette::kAccentHover},
        {"${accent-pressed}", ExoSnapPalette::kAccentPressed},
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

QString BuildThemeStylesheet(const ThemeFontFamilies& font_families) {
    QString qss = ReadThemeQss();
    if (qss.isEmpty())
        return qss;

    for (const auto& token : BuildTokens(font_families))
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

void ApplyPalette(QApplication& app) {
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(ExoSnapPalette::kBg0));
    palette.setColor(QPalette::WindowText, QColor(ExoSnapPalette::kText0));
    palette.setColor(QPalette::Base, QColor(ExoSnapPalette::kBg2));
    palette.setColor(QPalette::AlternateBase, QColor(ExoSnapPalette::kBg3));
    palette.setColor(QPalette::Text, QColor(ExoSnapPalette::kText0));
    palette.setColor(QPalette::Button, QColor(ExoSnapPalette::kBg3));
    palette.setColor(QPalette::ButtonText, QColor(ExoSnapPalette::kText1));
    palette.setColor(QPalette::Highlight, QColor(ExoSnapPalette::kAccent));
    palette.setColor(QPalette::HighlightedText, QColor(ExoSnapPalette::kAccentInk));
    palette.setColor(QPalette::Link, QColor(ExoSnapPalette::kAccent));
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
    app.setStyle("Fusion");
    ApplyDefaultAppFont(app, font_families);
    ApplyPalette(app);
    app.setStyleSheet(BuildThemeStylesheet(font_families));
}

} // namespace exosnap::ui::theme
