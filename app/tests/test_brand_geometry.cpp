// The drift guard between the canonical mark geometry and the artefacts checked
// in beside it.
//
// ui/brand/BrandMark.h is the source of truth, and two of its three consumers
// cannot drift from it by construction: the runtime renderer includes it, and
// ExoBrandMark.qml binds to the singleton that publishes it. The third is a
// generator that runs by hand, and its output -- exosnap-logo.svg -- is a file
// somebody can edit.
//
// So this reads both AS TEXT. Including the header and then also parsing it is
// the point: a test that resolved the SVG through the same constants it is
// checking would agree with itself no matter what the file on disk says.

#include "ui/brand/BrandMark.h"

#include <QFile>
#include <QRegularExpression>
#include <QString>

#include <gtest/gtest.h>

namespace brand = exosnap::ui::brand;

namespace {

QString ReadRepoFile(const QString& relative) {
    QFile file(QStringLiteral(EXOSNAP_BRAND_SOURCE_DIR) + QLatin1Char('/') + relative);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

// The nth <circle> element's attribute, as the file spells it.
QString CircleAttribute(const QString& svg, int index, const QString& attribute) {
    const QRegularExpression circles(QStringLiteral("<circle\\b[^>]*>"));
    QRegularExpressionMatchIterator it = circles.globalMatch(svg);
    int seen = 0;
    while (it.hasNext()) {
        const QString element = it.next().captured(0);
        if (seen++ != index)
            continue;
        const QRegularExpression attr(attribute + QStringLiteral("=\"([^\"]*)\""));
        const QRegularExpressionMatch match = attr.match(element);
        return match.hasMatch() ? match.captured(1) : QString();
    }
    return {};
}

void ExpectAttribute(const QString& svg, int circle, const QString& attribute, double expected) {
    const QString raw = CircleAttribute(svg, circle, attribute);
    ASSERT_FALSE(raw.isEmpty()) << "circle " << circle << " has no " << attribute.toStdString();
    bool ok = false;
    const double value = raw.toDouble(&ok);
    ASSERT_TRUE(ok) << raw.toStdString();
    EXPECT_DOUBLE_EQ(value, expected)
        << attribute.toStdString() << " on circle " << circle
        << " no longer matches ui/brand/BrandMark.h. Re-run scripts/generate-app-icons.py.";
}

} // namespace

TEST(BrandGeometry, TheCheckedInSvgIsStillTheCanonicalMark) {
    const QString svg = ReadRepoFile(QStringLiteral("assets/brand/exosnap-logo.svg"));
    ASSERT_FALSE(svg.isEmpty()) << "assets/brand/exosnap-logo.svg is missing or unreadable";

    EXPECT_TRUE(svg.contains(QStringLiteral("viewBox=\"0 0 32 32\"")));

    ExpectAttribute(svg, 0, QStringLiteral("cx"), brand::kCenter);
    ExpectAttribute(svg, 0, QStringLiteral("cy"), brand::kCenter);
    ExpectAttribute(svg, 0, QStringLiteral("r"), brand::kOuterRadius);
    ExpectAttribute(svg, 0, QStringLiteral("stroke-width"), brand::kOuterStroke);
    ExpectAttribute(svg, 0, QStringLiteral("opacity"), brand::kOuterOpacity);

    ExpectAttribute(svg, 1, QStringLiteral("r"), brand::kInnerRadius);
    ExpectAttribute(svg, 1, QStringLiteral("stroke-width"), brand::kInnerStroke);

    ExpectAttribute(svg, 2, QStringLiteral("r"), brand::kDotRadius);
}

TEST(BrandGeometry, TheGeneratorStillFindsEveryConstantItReads) {
    // The generator parses the header rather than restating it, which only holds
    // while the header keeps the shape it parses: one `inline constexpr double
    // kName = value;` per line, and designated initializers in the profiles. A
    // rename that broke that would fail at the next icon regeneration, which is
    // months after the change; this fails now.
    const QString header = ReadRepoFile(QStringLiteral("ui/brand/BrandMark.h"));
    ASSERT_FALSE(header.isEmpty());

    for (const QString& name :
         {QStringLiteral("kGrid"), QStringLiteral("kCenter"), QStringLiteral("kOuterRadius"),
          QStringLiteral("kOuterStroke"), QStringLiteral("kOuterOpacity"), QStringLiteral("kInnerRadius"),
          QStringLiteral("kInnerStroke"), QStringLiteral("kDotRadius"), QStringLiteral("kStandaloneContentScale"),
          QStringLiteral("kGlyphDiscRadius"), QStringLiteral("kGlyphSquareHalf"), QStringLiteral("kGlyphBarWidth"),
          QStringLiteral("kGlyphBarHeight"), QStringLiteral("kGlyphBarGap"), QStringLiteral("kGlyphTriangleBackX"),
          QStringLiteral("kGlyphTriangleTipX"), QStringLiteral("kGlyphTriangleHalfHeight"),
          QStringLiteral("kSmallProfileMaxPx"), QStringLiteral("kMediumProfileMaxPx")}) {
        const QRegularExpression declaration(
            QStringLiteral("^inline constexpr (?:double|int) %1 = [-\\d.]+;$").arg(name),
            QRegularExpression::MultilineOption);
        EXPECT_TRUE(declaration.match(header).hasMatch())
            << name.toStdString() << " is not in the one-per-line form scripts/generate-app-icons.py parses";
    }

    for (const QString& profile :
         {QStringLiteral("kSmallProfile"), QStringLiteral("kMediumProfile"), QStringLiteral("kLargeProfile")}) {
        const QRegularExpression declaration(QStringLiteral("inline constexpr OpticalProfile %1\\{").arg(profile));
        EXPECT_TRUE(declaration.match(header).hasMatch()) << profile.toStdString() << " is not parseable";
    }
    EXPECT_TRUE(header.contains(QStringLiteral(".outer_stroke_scale = ")));
    EXPECT_TRUE(header.contains(QStringLiteral(".inner_radius_scale = ")));
}

TEST(BrandGeometry, TheShellIconsThatAreStillFilesAreThere) {
    // What the runtime renderer does NOT paint: the executable's own identity,
    // and the thumbnail toolbar, whose THUMBBUTTON::hIcon takes a handle out of
    // the PE resource table rather than a QImage.
    for (const QString& asset : {QStringLiteral("exosnap-app.ico"), QStringLiteral("exosnap-thumb-record.ico"),
                                 QStringLiteral("exosnap-thumb-pause.ico"), QStringLiteral("exosnap-thumb-resume.ico"),
                                 QStringLiteral("exosnap-thumb-stop.ico")}) {
        QFile file(QStringLiteral(EXOSNAP_BRAND_SOURCE_DIR) + QStringLiteral("/assets/brand/") + asset);
        EXPECT_TRUE(file.exists()) << asset.toStdString() << " is referenced by exosnap.rc but is not on disk";
    }
}

TEST(BrandGeometry, NoPerStateApplicationIconSurvived) {
    // The state marks are painted at runtime now, from the accent the user
    // picked. A reappearing file here means somebody added a palette variant back
    // as an asset, which is the file-count spiral the renderer replaced -- or put
    // the taskbar's state back on an overlay badge, which said something
    // different from the tray about one session.
    for (const QString& gone :
         {QStringLiteral("exosnap-logo-idle.ico"), QStringLiteral("exosnap-logo-recording.ico"),
          QStringLiteral("exosnap-logo-paused.ico"), QStringLiteral("exosnap-logo-saved.ico"),
          QStringLiteral("exosnap-logo-recording-p0.ico"), QStringLiteral("exosnap-logo-recording-p1.ico"),
          QStringLiteral("exosnap-badge-recording.ico"), QStringLiteral("exosnap-badge-recording-dim.ico"),
          QStringLiteral("exosnap-badge-paused.ico"), QStringLiteral("exosnap-badge-saved.ico")}) {
        QFile file(QStringLiteral(EXOSNAP_BRAND_SOURCE_DIR) + QStringLiteral("/assets/brand/") + gone);
        EXPECT_FALSE(file.exists()) << gone.toStdString() << " is a per-state icon asset";
    }
}
