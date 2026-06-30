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

#include "ExoSnapMetrics.h"
#include "ExoSnapPalette.h"
#include "ExoSnapThemes.h"

namespace exosnap::ui::theme {
namespace {

// ---------------------------------------------------------------------------
// Compile-time string comparison helper
// ---------------------------------------------------------------------------

constexpr bool CStrEqual(const char* a, const char* b) {
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

// ---------------------------------------------------------------------------
// Verify dark-default values match the legacy ExoSnapPalette constants
// ---------------------------------------------------------------------------

static_assert(CStrEqual(kExoThemes.front().id, "dark-default"), "First theme must be dark-default.");
static_assert(CStrEqual(kExoThemes.front().bg, ExoSnapPalette::kBg0), "dark-default bg must match kBg0.");
static_assert(CStrEqual(kExoThemes.front().surf, ExoSnapPalette::kBg1), "dark-default surf must match kBg1.");
static_assert(CStrEqual(kExoThemes.front().surf2, ExoSnapPalette::kBg2), "dark-default surf2 must match kBg2.");
static_assert(CStrEqual(kExoThemes.front().raise, ExoSnapPalette::kBg3), "dark-default raise must match kBg3.");
static_assert(CStrEqual(kExoThemes.front().ink, ExoSnapPalette::kText0), "dark-default ink must match kText0.");
static_assert(CStrEqual(kExoThemes.front().mut, ExoSnapPalette::kText2), "dark-default mut must match kText2.");
static_assert(CStrEqual(kExoThemes.front().dim, ExoSnapPalette::kText3), "dark-default dim must match kText3.");
static_assert(CStrEqual(kExoThemes.front().ac, ExoSnapPalette::kAccent), "dark-default ac must match kAccent.");
static_assert(CStrEqual(kExoThemes.front().ac_ink, ExoSnapPalette::kAccentInk),
              "dark-default ac_ink must match kAccentInk.");
static_assert(CStrEqual(kExoThemes.front().success, ExoSnapPalette::kOk), "dark-default success must match kOk.");
static_assert(CStrEqual(kExoThemes.front().caution, ExoSnapPalette::kWarn), "dark-default caution must match kWarn.");
static_assert(CStrEqual(kExoThemes.front().error, ExoSnapPalette::kErr), "dark-default error must match kErr.");
// Derived override cross-checks:
static_assert(CStrEqual(kExoThemes.front().bg4_override, ExoSnapPalette::kBg4),
              "dark-default bg4_override must match kBg4.");
static_assert(CStrEqual(kExoThemes.front().line3_override, ExoSnapPalette::kLine3),
              "dark-default line3_override must match kLine3.");
static_assert(CStrEqual(kExoThemes.front().text1_override, ExoSnapPalette::kText1),
              "dark-default text1_override must match kText1.");

// ---------------------------------------------------------------------------
// Colour math helpers
// ---------------------------------------------------------------------------

// Derived alpha token: "rgba(R, G, B, alpha)" from a QColor.
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

// Blend c1 toward c2 at `t` (0=c1, 1=c2).
QColor Blend(const QColor& c1, const QColor& c2, double t) {
    const double s = std::clamp(t, 0.0, 1.0);
    return QColor(qRound(c1.red() * (1.0 - s) + c2.red() * s), qRound(c1.green() * (1.0 - s) + c2.green() * s),
                  qRound(c1.blue() * (1.0 - s) + c2.blue() * s));
}

// ---------------------------------------------------------------------------
// Theme resolution
// ---------------------------------------------------------------------------

// Find a theme by id; returns nullptr if not found.
const ExoTheme* FindTheme(const QString& theme_id) {
    for (const auto& t : kExoThemes) {
        if (QString::fromUtf8(t.id) == theme_id)
            return &t;
    }
    return nullptr;
}

// Resolve a theme by id, falling back to dark-default when not found.
const ExoTheme& ResolveTheme(const QString& theme_id) {
    if (const ExoTheme* t = FindTheme(theme_id))
        return *t;
    qWarning().noquote() << "ExoSnap: unknown theme id" << theme_id << "— falling back to dark-default.";
    return kExoThemes.front();
}

// ---------------------------------------------------------------------------
// Active theme storage
// ---------------------------------------------------------------------------

ExoTheme& ActiveThemeStorage() {
    static ExoTheme active = kExoThemes.front();
    return active;
}

// ---------------------------------------------------------------------------
// Font family helpers
// ---------------------------------------------------------------------------

struct ThemeFontFamilies {
    // Hanken Grotesk is the design-system UI face (bundled, OFL); LoadBundledFonts
    // sets this to the loaded family, falling back to Segoe UI if it fails to load.
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
    // Hanken Grotesk (bundled, design-system UI face) leads; Segoe UI is the
    // system fallback if the bundled font ever fails to load.
    return BuildFamilyStack(font_families.sans_primary,
                            {QStringLiteral("Segoe UI Variable Text"), QStringLiteral("Segoe UI"),
                             QStringLiteral("system-ui"), QStringLiteral("sans-serif")});
}

QStringList MonoFamilies(const ThemeFontFamilies& font_families) {
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

// ---------------------------------------------------------------------------
// QSS loading + token replacement
// ---------------------------------------------------------------------------

struct ThemeToken {
    QString key;
    QString value;
};

QString ReadThemeQss() {
    QFile file(":/theme/exosnap_dark.qss");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning("Failed to load ExoSnap theme QSS resource.");
        return {};
    }
    QTextStream stream(&file);
    return stream.readAll();
}

QVector<ThemeToken> BuildTokens(const ThemeFontFamilies& font_families, const ExoTheme& theme) {
    const bool dark = (theme.kind == ThemeKind::Dark);

    // Resolve colours from theme fields
    const QColor bg4_c = theme.bg4_override ? QColor(QString::fromUtf8(theme.bg4_override))
                                            : (dark ? Lighten(QColor(QString::fromUtf8(theme.raise)), 0.10)
                                                    : Darken(QColor(QString::fromUtf8(theme.raise)), 0.04));

    const QColor ink_c(QString::fromUtf8(theme.ink));
    const QColor mut_c(QString::fromUtf8(theme.mut));
    const QColor dim_c(QString::fromUtf8(theme.dim));
    const QColor ac_c(QString::fromUtf8(theme.ac));
    const QColor ac2_c(QString::fromUtf8(theme.ac2));
    const QColor ok_c(QString::fromUtf8(theme.success));
    const QColor warn_c(QString::fromUtf8(theme.caution));
    const QColor err_c(QString::fromUtf8(theme.error));

    // line3 (stronger hover border)
    QString line3;
    if (theme.line3_override) {
        line3 = QString::fromUtf8(theme.line3_override);
    } else {
        line3 = dark ? QStringLiteral("rgba(255, 255, 255, 0.20)") : RgbaToken(ink_c, 0.24);
    }

    // text1 (body color)
    QString text1;
    if (theme.text1_override) {
        text1 = QString::fromUtf8(theme.text1_override);
    } else {
        text1 = Blend(ink_c, mut_c, 0.42).name();
    }

    // Alpha sets
    const double acDim = dark ? 0.14 : 0.12;
    const double acSoft = dark ? 0.24 : 0.18;
    const double acB1 = dark ? 0.42 : 0.34;
    const double acB2 = dark ? 0.60 : 0.52;
    const double ac2Dim = dark ? 0.16 : 0.13;
    const double ac2B = dark ? 0.55 : 0.46;
    const double sDim = dark ? 0.13 : 0.12;
    const double sB = dark ? 0.44 : 0.42;
    const double recDim = dark ? 0.16 : 0.13;

    return {
        // Backgrounds
        {"${bg0}", QString::fromUtf8(theme.bg)},
        {"${bg1}", QString::fromUtf8(theme.surf)},
        {"${bg2}", QString::fromUtf8(theme.surf2)},
        {"${bg3}", QString::fromUtf8(theme.raise)},
        {"${bg4}", bg4_c.name()},
        // Lines
        {"${line1}", QString::fromUtf8(theme.line)},
        {"${line2}", QString::fromUtf8(theme.line2)},
        {"${line3}", line3},
        // Text
        {"${text0}", QString::fromUtf8(theme.ink)},
        {"${text1}", text1},
        {"${text2}", QString::fromUtf8(theme.mut)},
        {"${text3}", QString::fromUtf8(theme.dim)},
        // Primary accent
        {"${accent}", QString::fromUtf8(theme.ac)},
        {"${accent-ink}", QString::fromUtf8(theme.ac_ink)},
        {"${accent-dim}", RgbaToken(ac_c, acDim)},
        {"${accent-soft}", RgbaToken(ac_c, acSoft)},
        {"${accent-line}", RgbaToken(ac_c, acB1)},
        {"${accent-b2}", RgbaToken(ac_c, acB2)},
        {"${accent-hover}", Lighten(ac_c, 0.14).name()},
        {"${accent-pressed}", Darken(ac_c, 0.09).name()},
        // Secondary accent
        {"${accent2}", QString::fromUtf8(theme.ac2)},
        {"${accent2-ink}", QString::fromUtf8(theme.ac2_ink)},
        {"${accent2-dim}", RgbaToken(ac2_c, ac2Dim)},
        {"${accent2-b2}", RgbaToken(ac2_c, ac2B)},
        // Semantic
        {"${ok}", QString::fromUtf8(theme.success)},
        {"${warn}", QString::fromUtf8(theme.caution)},
        {"${err}", QString::fromUtf8(theme.error)},
        {"${info}", QString::fromUtf8(theme.ac)}, // info = accent
        {"${ok-dim}", RgbaToken(ok_c, sDim)},
        {"${ok-b}", RgbaToken(ok_c, sB)},
        {"${warn-dim}", RgbaToken(warn_c, sDim)},
        {"${warn-b}", RgbaToken(warn_c, sB)},
        {"${err-dim}", RgbaToken(err_c, sDim)},
        {"${err-b}", RgbaToken(err_c, sB)},
        {"${rec-dim}", RgbaToken(err_c, recDim)},
        // ok alpha tokens (fixed alpha, base color follows theme)
        {"${ok-a06}", RgbaToken(ok_c, 0.06)},
        {"${ok-a07}", RgbaToken(ok_c, 0.07)},
        {"${ok-a08}", RgbaToken(ok_c, 0.08)},
        {"${ok-a12}", RgbaToken(ok_c, 0.12)},
        {"${ok-a13}", RgbaToken(ok_c, 0.13)},
        {"${ok-a14}", RgbaToken(ok_c, 0.14)},
        {"${ok-a20}", RgbaToken(ok_c, 0.20)},
        {"${ok-a34}", RgbaToken(ok_c, 0.34)},
        {"${ok-a36}", RgbaToken(ok_c, 0.36)},
        {"${ok-a40}", RgbaToken(ok_c, 0.40)},
        {"${ok-a42}", RgbaToken(ok_c, 0.42)},
        {"${ok-a44}", RgbaToken(ok_c, 0.44)},
        {"${ok-a46}", RgbaToken(ok_c, 0.46)},
        {"${ok-a48}", RgbaToken(ok_c, 0.48)},
        {"${ok-a52}", RgbaToken(ok_c, 0.52)},
        {"${ok-a68}", RgbaToken(ok_c, 0.68)},
        // warn alpha tokens (fixed alpha, base color follows theme)
        {"${warn-a08}", RgbaToken(warn_c, 0.08)},
        {"${warn-a10}", RgbaToken(warn_c, 0.10)},
        {"${warn-a12}", RgbaToken(warn_c, 0.12)},
        {"${warn-a14}", RgbaToken(warn_c, 0.14)},
        {"${warn-a15}", RgbaToken(warn_c, 0.15)},
        {"${warn-a16}", RgbaToken(warn_c, 0.16)},
        {"${warn-a18}", RgbaToken(warn_c, 0.18)},
        {"${warn-a20}", RgbaToken(warn_c, 0.20)},
        {"${warn-a22}", RgbaToken(warn_c, 0.22)},
        {"${warn-a24}", RgbaToken(warn_c, 0.24)},
        {"${warn-a42}", RgbaToken(warn_c, 0.42)},
        {"${warn-a44}", RgbaToken(warn_c, 0.44)},
        {"${warn-a48}", RgbaToken(warn_c, 0.48)},
        {"${warn-a50}", RgbaToken(warn_c, 0.50)},
        {"${warn-a52}", RgbaToken(warn_c, 0.52)},
        {"${warn-a58}", RgbaToken(warn_c, 0.58)},
        {"${warn-a64}", RgbaToken(warn_c, 0.64)},
        {"${warn-a68}", RgbaToken(warn_c, 0.68)},
        // err alpha tokens (fixed alpha, base color follows theme)
        {"${err-a08}", RgbaToken(err_c, 0.08)},
        {"${err-a10}", RgbaToken(err_c, 0.10)},
        {"${err-a12}", RgbaToken(err_c, 0.12)},
        {"${err-a16}", RgbaToken(err_c, 0.16)},
        {"${err-a18}", RgbaToken(err_c, 0.18)},
        {"${err-a20}", RgbaToken(err_c, 0.20)},
        {"${err-a22}", RgbaToken(err_c, 0.22)},
        {"${err-a42}", RgbaToken(err_c, 0.42)},
        {"${err-a48}", RgbaToken(err_c, 0.48)},
        {"${err-a52}", RgbaToken(err_c, 0.52)},
        {"${err-a55}", RgbaToken(err_c, 0.55)},
        {"${err-a58}", RgbaToken(err_c, 0.58)},
        {"${err-a60}", RgbaToken(err_c, 0.60)},
        {"${err-a62}", RgbaToken(err_c, 0.62)},
        {"${err-a66}", RgbaToken(err_c, 0.66)},
        {"${err-a70}", RgbaToken(err_c, 0.70)},
        {"${err-a74}", RgbaToken(err_c, 0.74)},
        // accent alpha tokens (fixed alpha, base color follows theme)
        {"${ac-a10}", RgbaToken(ac_c, 0.10)},
        {"${ac-a12}", RgbaToken(ac_c, 0.12)},
        {"${ac-a16}", RgbaToken(ac_c, 0.16)},
        {"${ac-a18}", RgbaToken(ac_c, 0.18)},
        // surface alpha tokens (chip/overlay backgrounds derived from surface colors)
        {"${surf-a92}", RgbaToken(QColor(QString::fromUtf8(theme.surf)), 0.92)},
        {"${surf2-a78}", RgbaToken(QColor(QString::fromUtf8(theme.surf2)), 0.78)},
        {"${surf2-a86}", RgbaToken(QColor(QString::fromUtf8(theme.surf2)), 0.86)},
        {"${surf2-a92}", RgbaToken(QColor(QString::fromUtf8(theme.surf2)), 0.92)},
        // Log palette
        {"${log-cat}", QString::fromUtf8(theme.log.cat)},
        {"${log-info}", QString::fromUtf8(theme.log.info)},
        {"${log-warn}", QString::fromUtf8(theme.log.warn)},
        {"${log-error}", QString::fromUtf8(theme.log.error)},
        {"${log-debug}", QString::fromUtf8(theme.log.debug)},
        {"${log-time}", QString::fromUtf8(theme.log.time)},
        // Spacing / radius / sizing
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

QString BuildThemeStylesheet(const ThemeFontFamilies& font_families, const ExoTheme& theme) {
    QString qss = ReadThemeQss();
    if (qss.isEmpty())
        return qss;

    for (const auto& token : BuildTokens(font_families, theme))
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

// ---------------------------------------------------------------------------
// Qt palette application
// ---------------------------------------------------------------------------

void ApplyPalette(QApplication& app, const ExoTheme& theme) {
    const QColor ink(QString::fromUtf8(theme.ink));
    const QColor mut(QString::fromUtf8(theme.mut));
    const QColor dim(QString::fromUtf8(theme.dim));
    const QColor ac(QString::fromUtf8(theme.ac));
    const QColor ac_ink(QString::fromUtf8(theme.ac_ink));

    // Derive text1 for ButtonText
    const QString text1 = theme.text1_override ? QString::fromUtf8(theme.text1_override) : Blend(ink, mut, 0.42).name();

    QPalette palette;
    palette.setColor(QPalette::Window, QColor(QString::fromUtf8(theme.bg)));
    palette.setColor(QPalette::WindowText, ink);
    palette.setColor(QPalette::Base, QColor(QString::fromUtf8(theme.surf2)));
    palette.setColor(QPalette::AlternateBase, QColor(QString::fromUtf8(theme.raise)));
    palette.setColor(QPalette::Text, ink);
    palette.setColor(QPalette::Button, QColor(QString::fromUtf8(theme.raise)));
    palette.setColor(QPalette::ButtonText, QColor(text1));
    palette.setColor(QPalette::Highlight, ac);
    palette.setColor(QPalette::HighlightedText, ac_ink);
    palette.setColor(QPalette::Link, ac);
    palette.setColor(QPalette::ToolTipBase, QColor(QString::fromUtf8(theme.surf2)));
    palette.setColor(QPalette::ToolTipText, ink);
    palette.setColor(QPalette::PlaceholderText, mut);
    palette.setColor(QPalette::Disabled, QPalette::Text, dim);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, dim);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, dim);
    app.setPalette(palette);
}

// ---------------------------------------------------------------------------
// Font loading
// ---------------------------------------------------------------------------

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

    const QStringList plex_families = LoadFontFamiliesFromResources(
        {QStringLiteral(":/fonts/IBMPlexMono-Regular.ttf"), QStringLiteral(":/fonts/IBMPlexMono-Medium.ttf")},
        QStringLiteral("IBM Plex Mono"));

    font_families.mono_primary = SelectPreferredFamily(plex_families, font_families.mono_primary);

    // Hanken Grotesk — the design-system UI face (OFL, bundled). Regular/Medium/
    // SemiBold/Bold cover the weights the UI uses (body, DemiBold titles, brand).
    const QStringList hanken_families = LoadFontFamiliesFromResources(
        {QStringLiteral(":/fonts/HankenGrotesk-Regular.ttf"), QStringLiteral(":/fonts/HankenGrotesk-Medium.ttf"),
         QStringLiteral(":/fonts/HankenGrotesk-SemiBold.ttf"), QStringLiteral(":/fonts/HankenGrotesk-Bold.ttf")},
        QStringLiteral("Hanken Grotesk"));

    font_families.sans_primary = SelectPreferredFamily(hanken_families, font_families.sans_primary);
    return font_families;
}

void ApplyDefaultAppFont(QApplication& app, const ThemeFontFamilies& font_families) {
    QFont app_font;
    app_font.setFamilies(SansFamilies(font_families));
    app_font.setPointSize(10);
    app.setFont(app_font);
}

// Cached font families after initial load; populated by ApplyExoSnapTheme().
ThemeFontFamilies& CachedFontFamilies() {
    static ThemeFontFamilies font_families;
    return font_families;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

const ExoTheme& ActiveTheme() {
    return ActiveThemeStorage();
}

QColor ParseThemeColor(const char* css_color) {
    if (!css_color)
        return {};

    const QString s = QString::fromUtf8(css_color).trimmed();

    // Try direct QColor parse (handles #rrggbb, #rgb, named colors, etc.)
    QColor c(s);
    if (c.isValid())
        return c;

    // Try rgba(r, g, b, a) where a is 0..1 float
    static const QRegularExpression kRgba(R"(rgba\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*([0-9]*\.?[0-9]+)\s*\))",
                                          QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = kRgba.match(s);
    if (m.hasMatch()) {
        const int r = m.captured(1).toInt();
        const int g = m.captured(2).toInt();
        const int b = m.captured(3).toInt();
        const double a = m.captured(4).toDouble();
        return QColor(r, g, b, qRound(a * 255.0));
    }

    return {};
}

void ApplyExoSnapTheme(QApplication& app) {
    const ThemeFontFamilies font_families = LoadBundledFonts();
    CachedFontFamilies() = font_families;

    const ExoTheme& theme = kExoThemes.front(); // dark-default
    ActiveThemeStorage() = theme;

    app.setStyle("Fusion");
    ApplyDefaultAppFont(app, font_families);
    ApplyPalette(app, theme);
    app.setStyleSheet(BuildThemeStylesheet(font_families, theme));
}

void ReapplyTheme(QApplication& app, const QString& theme_id) {
    const ExoTheme& theme = ResolveTheme(theme_id);
    ActiveThemeStorage() = theme;

    ApplyPalette(app, theme);
    app.setStyleSheet(BuildThemeStylesheet(CachedFontFamilies(), theme));
}

QString ThemeBg4Color(const ExoTheme& theme) {
    if (theme.bg4_override)
        return QString::fromUtf8(theme.bg4_override);
    const QColor raise(QString::fromUtf8(theme.raise));
    const bool dark = (theme.kind == ThemeKind::Dark);
    const double f = dark ? 0.10 : 0.04;
    const QColor bg4 =
        dark ? QColor(qRound(raise.red() + (255 - raise.red()) * f), qRound(raise.green() + (255 - raise.green()) * f),
                      qRound(raise.blue() + (255 - raise.blue()) * f))
             : QColor(qRound(raise.red() * (1.0 - f)), qRound(raise.green() * (1.0 - f)),
                      qRound(raise.blue() * (1.0 - f)));
    return bg4.name();
}

QString ThemeText1Color(const ExoTheme& theme) {
    if (theme.text1_override)
        return QString::fromUtf8(theme.text1_override);
    const QColor ink(QString::fromUtf8(theme.ink));
    const QColor mut(QString::fromUtf8(theme.mut));
    const double t = 0.42;
    return QColor(qRound(ink.red() * (1.0 - t) + mut.red() * t), qRound(ink.green() * (1.0 - t) + mut.green() * t),
                  qRound(ink.blue() * (1.0 - t) + mut.blue() * t))
        .name();
}

QString ThemeAccentHover(const ExoTheme& theme) {
    const QColor ac(QString::fromUtf8(theme.ac));
    constexpr double f = 0.14;
    return QColor(qRound(ac.red() + (255 - ac.red()) * f), qRound(ac.green() + (255 - ac.green()) * f),
                  qRound(ac.blue() + (255 - ac.blue()) * f))
        .name();
}

QString ThemeAccentPressed(const ExoTheme& theme) {
    const QColor ac(QString::fromUtf8(theme.ac));
    constexpr double f = 0.09;
    return QColor(qRound(ac.red() * (1.0 - f)), qRound(ac.green() * (1.0 - f)), qRound(ac.blue() * (1.0 - f))).name();
}

QString ThemeRgba(const QColor& base, double alpha) {
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(base.red())
        .arg(base.green())
        .arg(base.blue())
        .arg(QString::number(alpha, 'f', 2));
}

} // namespace exosnap::ui::theme
