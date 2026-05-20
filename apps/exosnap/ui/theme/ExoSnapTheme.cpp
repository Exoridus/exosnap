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

#include "ExoSnapMetrics.h"
#include "ExoSnapPalette.h"

namespace exosnap::ui::theme {
namespace {

struct ThemeToken {
    QString key;
    QString value;
};

struct ThemeFontFamilies {
    QString sans_primary = QStringLiteral("Inter");
    QString mono_primary = QStringLiteral("JetBrains Mono");
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
    return BuildFamilyStack(font_families.sans_primary, {QStringLiteral("Segoe UI"), QStringLiteral("sans-serif")});
}

QStringList MonoFamilies(const ThemeFontFamilies& font_families) {
    return BuildFamilyStack(font_families.mono_primary,
                            {QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"), QStringLiteral("monospace")});
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
        {"${accent-dim}", ExoSnapPalette::kAccentDim},
        {"${accent-line}", ExoSnapPalette::kAccentLine},
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
    palette.setColor(QPalette::HighlightedText, QColor(ExoSnapPalette::kBg0));
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

    const QStringList inter_families = LoadFontFamiliesFromResources({QStringLiteral(":/fonts/Inter-Regular.ttf"),
                                                                      QStringLiteral(":/fonts/Inter-Medium.ttf"),
                                                                      QStringLiteral(":/fonts/Inter-SemiBold.ttf")},
                                                                     QStringLiteral("Inter"));
    const QStringList jetbrains_families = LoadFontFamiliesFromResources(
        {QStringLiteral(":/fonts/JetBrainsMono-Regular.ttf"), QStringLiteral(":/fonts/JetBrainsMono-Medium.ttf"),
         QStringLiteral(":/fonts/JetBrainsMono-SemiBold.ttf")},
        QStringLiteral("JetBrains Mono"));

    font_families.sans_primary = SelectPreferredFamily(inter_families, font_families.sans_primary);
    font_families.mono_primary = SelectPreferredFamily(jetbrains_families, font_families.mono_primary);
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
