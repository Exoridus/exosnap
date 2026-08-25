// The drift guard between the canonical brand parameters, the mark suite
// generated from them, and the constants the runtime substitutes into it.
//
// The suite in app/assets/brand/marks is written by
// scripts/generate-brand-marks.py from marks/parameters.json. Nothing at build
// time re-runs that script, so the assets are files somebody can edit -- and the
// runtime's substitution table is a set of literals that has to keep matching
// what the generator writes.
//
// So this reads the assets, the parameters and the header AS TEXT. Resolving any
// of them through the others would produce a test that agrees with itself no
// matter what is on disk.

#include "ui/brand/BrandMark.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

#include <gtest/gtest.h>

namespace brand = exosnap::ui::brand;

namespace {

// The two sequences have different lengths: the recording beat rests at the
// bottom of its loop for two ticks, and the processing arc has no rest.
constexpr int kRecordingFrameCount = 6;
constexpr int kProcessingFrameCount = 4;

QString ReadRepoFile(const QString& relative) {
    QFile file(QStringLiteral(EXOSNAP_BRAND_SOURCE_DIR) + QLatin1Char('/') + relative);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

QString MarkPath(const QString& stem) {
    return QStringLiteral("assets/brand/marks/%1.svg").arg(stem);
}

// Every drawing in the suite, by stem, in one list so a test can sweep them.
QStringList MarkStems() {
    QStringList stems{QStringLiteral("brand"), QStringLiteral("idle"),    QStringLiteral("paused"),
                      QStringLiteral("saved"), QStringLiteral("warning"), QStringLiteral("error")};
    for (int frame = 0; frame < kRecordingFrameCount; ++frame)
        stems << QStringLiteral("recording-f%1").arg(frame);
    for (int frame = 0; frame < kProcessingFrameCount; ++frame)
        stems << QStringLiteral("processing-f%1").arg(frame);
    return stems;
}

QJsonObject Parameters() {
    const QString text = ReadRepoFile(QStringLiteral("assets/brand/marks/parameters.json"));
    return QJsonDocument::fromJson(text.toUtf8()).object();
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

void ExpectAttribute(const QString& svg, const QString& stem, int circle, const QString& attribute, double expected) {
    const QString raw = CircleAttribute(svg, circle, attribute);
    ASSERT_FALSE(raw.isEmpty()) << stem.toStdString() << ": circle " << circle << " has no " << attribute.toStdString();
    bool ok = false;
    const double value = raw.toDouble(&ok);
    ASSERT_TRUE(ok) << raw.toStdString();
    EXPECT_DOUBLE_EQ(value, expected)
        << stem.toStdString() << ": " << attribute.toStdString() << " on circle " << circle
        << " no longer matches marks/parameters.json. Re-run scripts/generate-brand-marks.py.";
}

} // namespace

TEST(BrandGeometry, EveryMarkInTheSuiteIsOnDisk) {
    for (const QString& stem : MarkStems()) {
        QFile file(QStringLiteral(EXOSNAP_BRAND_SOURCE_DIR) + QLatin1Char('/') + MarkPath(stem));
        EXPECT_TRUE(file.exists()) << stem.toStdString() << " is in the suite the runtime asks for but not on disk";
    }
}

TEST(BrandGeometry, NoMarkOutsideTheSuiteIsShipped) {
    // The suite is generated, and an asset nobody generated is one nobody can
    // regenerate. It is also a mark the resource file does not list, so it would
    // ship as a file in the repository and as nothing in the binary.
    QDir dir(QStringLiteral(EXOSNAP_BRAND_SOURCE_DIR) + QStringLiteral("/assets/brand/marks"));
    const QStringList found = dir.entryList({QStringLiteral("*.svg")}, QDir::Files);
    QStringList expected;
    for (const QString& stem : MarkStems())
        expected << stem + QStringLiteral(".svg");
    expected.sort();
    QStringList sorted = found;
    sorted.sort();
    EXPECT_EQ(sorted, expected);
}

TEST(BrandGeometry, TheApertureIsTheOneInTheParameters) {
    const QJsonObject geometry = Parameters().value(QStringLiteral("geometry")).toObject();
    ASSERT_FALSE(geometry.isEmpty()) << "marks/parameters.json is missing or unreadable";

    for (const QString& stem : MarkStems()) {
        const QString svg = ReadRepoFile(MarkPath(stem));
        ASSERT_FALSE(svg.isEmpty()) << stem.toStdString();

        // The outer ring is the same in every drawing in the suite: it is what
        // makes them one mark rather than eight icons.
        ExpectAttribute(svg, stem, 0, QStringLiteral("cx"), brand::kCenter);
        ExpectAttribute(svg, stem, 0, QStringLiteral("cy"), brand::kCenter);
        ExpectAttribute(svg, stem, 0, QStringLiteral("r"), geometry.value(QStringLiteral("outer_r")).toDouble());
        ExpectAttribute(svg, stem, 0, QStringLiteral("stroke-width"),
                        geometry.value(QStringLiteral("outer_w")).toDouble());
        ExpectAttribute(svg, stem, 0, QStringLiteral("opacity"), brand::kReferenceOuterOpacity);
    }
}

TEST(BrandGeometry, TheInnerRingIsTheOneInTheParameters) {
    // Including every recording frame: the beat modulates brightness and NOTHING
    // else, which is what makes it affordable to run for the length of a
    // recording. A frame that moved the ring would differ from its neighbour by
    // well under a device pixel at 16 px, and that reads as a flicker.
    const QJsonObject geometry = Parameters().value(QStringLiteral("geometry")).toObject();
    QStringList stems{QStringLiteral("brand"), QStringLiteral("idle"),    QStringLiteral("paused"),
                      QStringLiteral("saved"), QStringLiteral("warning"), QStringLiteral("error")};
    for (int frame = 0; frame < kRecordingFrameCount; ++frame)
        stems << QStringLiteral("recording-f%1").arg(frame);
    for (const QString& stem : stems) {
        const QString svg = ReadRepoFile(MarkPath(stem));
        ASSERT_FALSE(svg.isEmpty()) << stem.toStdString();
        ExpectAttribute(svg, stem, 1, QStringLiteral("r"), geometry.value(QStringLiteral("inner_r")).toDouble());
        ExpectAttribute(svg, stem, 1, QStringLiteral("stroke-width"),
                        geometry.value(QStringLiteral("inner_w")).toDouble());
    }
}

TEST(BrandGeometry, EveryColourInTheSuiteIsOneTheRuntimeSubstitutes) {
    // The runtime replaces four exact strings. A fifth colour would ship
    // unrecoloured: the designer's mint in a user's violet accent, or a dark
    // theme's coral on a light appearance, with nothing to notice it.
    const QSet<QString> tokens{QString::fromLatin1(brand::kReferenceAccent).toUpper(),
                               QString::fromLatin1(brand::kReferenceRecording).toUpper(),
                               QString::fromLatin1(brand::kReferenceCaution).toUpper(),
                               QString::fromLatin1(brand::kReferenceSuccess).toUpper()};
    const QRegularExpression colour(QStringLiteral("#[0-9A-Fa-f]{6}"));
    for (const QString& stem : MarkStems()) {
        const QString svg = ReadRepoFile(MarkPath(stem));
        ASSERT_FALSE(svg.isEmpty()) << stem.toStdString();
        QRegularExpressionMatchIterator it = colour.globalMatch(svg);
        int seen = 0;
        while (it.hasNext()) {
            const QString value = it.next().captured(0).toUpper();
            ++seen;
            EXPECT_TRUE(tokens.contains(value)) << stem.toStdString() << " carries " << value.toStdString()
                                                << ", which ui/brand/BrandMarkSvg.cpp does not substitute";
        }
        EXPECT_GT(seen, 0) << stem.toStdString() << " has no colour at all";
    }
}

TEST(BrandGeometry, TheOuterOpacityIsSubstitutableExactlyOnce) {
    // The light appearance is the dark suite with one value replaced, which only
    // works while that value appears once and only on the outer ring. A second
    // occurrence would silently take the light theme's heavier ring with it.
    const QString literal = QStringLiteral("opacity=\"%1\"").arg(brand::kReferenceOuterOpacity);
    for (const QString& stem : MarkStems()) {
        const QString svg = ReadRepoFile(MarkPath(stem));
        ASSERT_FALSE(svg.isEmpty()) << stem.toStdString();
        EXPECT_EQ(svg.count(literal), 1) << stem.toStdString() << " does not carry " << literal.toStdString()
                                         << " exactly once";
    }
}

TEST(BrandGeometry, TheParametersAndTheRuntimeAgreeOnThePalette) {
    const QJsonObject parameters = Parameters();
    const QJsonObject colours = parameters.value(QStringLiteral("reference_colors")).toObject();
    ASSERT_FALSE(colours.isEmpty());

    EXPECT_EQ(colours.value(QStringLiteral("accent")).toString().toUpper(),
              QString::fromLatin1(brand::kReferenceAccent).toUpper());
    EXPECT_EQ(colours.value(QStringLiteral("recording")).toString().toUpper(),
              QString::fromLatin1(brand::kReferenceRecording).toUpper());
    EXPECT_EQ(colours.value(QStringLiteral("caution")).toString().toUpper(),
              QString::fromLatin1(brand::kReferenceCaution).toUpper());
    EXPECT_EQ(colours.value(QStringLiteral("success")).toString().toUpper(),
              QString::fromLatin1(brand::kReferenceSuccess).toUpper());

    const QJsonObject opacity = parameters.value(QStringLiteral("outer_opacity")).toObject();
    EXPECT_DOUBLE_EQ(opacity.value(QStringLiteral("dark")).toDouble(), brand::kReferenceOuterOpacity);
    EXPECT_DOUBLE_EQ(opacity.value(QStringLiteral("dark")).toDouble(), brand::kOuterOpacityDark);
    EXPECT_DOUBLE_EQ(opacity.value(QStringLiteral("light")).toDouble(), brand::kOuterOpacityLight);
    EXPECT_GT(brand::kOuterOpacityLight, brand::kOuterOpacityDark)
        << "the light appearance needs the heavier ring, not the lighter one";
}

TEST(BrandGeometry, ThePublishedLogoIsTheCanonicalMark) {
    // A stable path, pointed at by the package manifests. A copy of the asset
    // rather than a second drawing of it.
    const QString logo = ReadRepoFile(QStringLiteral("assets/brand/exosnap-logo.svg"));
    const QString mark = ReadRepoFile(MarkPath(QStringLiteral("brand")));
    ASSERT_FALSE(logo.isEmpty()) << "assets/brand/exosnap-logo.svg is missing or unreadable";
    EXPECT_EQ(logo, mark) << "exosnap-logo.svg has drifted from the mark. Re-run scripts/generate-app-icons.py.";
}

TEST(BrandGeometry, TheGeneratorStillFindsEveryConstantItReads) {
    // scripts/generate-app-icons.py parses the header rather than restating it,
    // which only holds while the header keeps the shape it parses: one `inline
    // constexpr double kName = value;` per line, and designated initializers in
    // the profiles. A rename that broke that would fail at the next icon
    // regeneration, which is months after the change; this fails now.
    const QString header = ReadRepoFile(QStringLiteral("ui/brand/BrandMark.h"));
    ASSERT_FALSE(header.isEmpty());

    for (const QString& name :
         {QStringLiteral("kGrid"), QStringLiteral("kCenter"), QStringLiteral("kStandaloneContentScale"),
          QStringLiteral("kGlyphDiscRadius"), QStringLiteral("kGlyphSquareHalf"), QStringLiteral("kGlyphBarWidth"),
          QStringLiteral("kGlyphBarHeight"), QStringLiteral("kGlyphBarGap"), QStringLiteral("kGlyphBarCorner"),
          QStringLiteral("kGlyphSquareCorner"), QStringLiteral("kGlyphTriangleBackX"),
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
    EXPECT_TRUE(header.contains(QStringLiteral(".ring_stroke_scale = ")));
    EXPECT_TRUE(header.contains(QStringLiteral(".outer_opacity_scale = ")));
    EXPECT_TRUE(header.contains(QStringLiteral(".content_scale = ")));
}

TEST(BrandGeometry, NoApertureGeometrySurvivedInTheHeader) {
    // The five numbers live in marks/parameters.json. A radius reintroduced here
    // is the second source of truth this architecture exists to prevent: the
    // header would then describe one mark and the assets another, and only one of
    // them is what the tray shows.
    const QString header = ReadRepoFile(QStringLiteral("ui/brand/BrandMark.h"));
    ASSERT_FALSE(header.isEmpty());
    for (const QString& gone :
         {QStringLiteral("kOuterRadius"), QStringLiteral("kOuterStroke"), QStringLiteral("kInnerRadius"),
          QStringLiteral("kInnerStroke"), QStringLiteral("kDotRadius")}) {
        EXPECT_FALSE(header.contains(gone))
            << gone.toStdString() << " is aperture geometry, which belongs in marks/parameters.json";
    }
}

TEST(BrandGeometry, TheShellIconsThatAreStillFilesAreThere) {
    // What the runtime renderer does NOT paint: the executable's own identity,
    // and the thumbnail toolbar, whose THUMBBUTTON::hIcon takes a handle out of
    // the PE resource table rather than a QImage.
    for (const QString& asset :
         {QStringLiteral("exosnap-app.ico"), QStringLiteral("exosnap-thumb-record.ico"),
          QStringLiteral("exosnap-thumb-pause.ico"), QStringLiteral("exosnap-thumb-resume.ico"),
          QStringLiteral("exosnap-thumb-stop.ico"), QStringLiteral("exosnap-thumb-folder.ico")}) {
        QFile file(QStringLiteral(EXOSNAP_BRAND_SOURCE_DIR) + QStringLiteral("/assets/brand/") + asset);
        EXPECT_TRUE(file.exists()) << asset.toStdString() << " is referenced by exosnap.rc but is not on disk";
    }
}

TEST(BrandGeometry, NoPerStateOrPerThemeIconAssetSurvived) {
    // The state marks are recoloured at runtime, from the accent the user picked
    // and the appearance they are in. A reappearing file here means somebody
    // added a palette variant back as an asset, which is the file-count spiral
    // the renderer exists to avoid.
    for (const QString& gone :
         {QStringLiteral("exosnap-logo-idle.ico"), QStringLiteral("exosnap-logo-recording.ico"),
          QStringLiteral("exosnap-logo-paused.ico"), QStringLiteral("exosnap-logo-saved.ico"),
          QStringLiteral("exosnap-logo-recording-p0.ico"), QStringLiteral("exosnap-logo-recording-p1.ico"),
          QStringLiteral("exosnap-badge-recording.ico"), QStringLiteral("exosnap-badge-recording-dim.ico"),
          QStringLiteral("exosnap-badge-paused.ico"), QStringLiteral("exosnap-badge-saved.ico")}) {
        QFile file(QStringLiteral(EXOSNAP_BRAND_SOURCE_DIR) + QStringLiteral("/assets/brand/") + gone);
        EXPECT_FALSE(file.exists()) << gone.toStdString() << " is a per-state icon asset";
    }
    for (const QString& gone : {QStringLiteral("dark"), QStringLiteral("light")}) {
        QDir dir(QStringLiteral(EXOSNAP_BRAND_SOURCE_DIR) + QStringLiteral("/assets/brand/marks/") + gone);
        EXPECT_FALSE(dir.exists()) << gone.toStdString()
                                   << " is a per-theme mark set; the two differ in one opacity and are substituted";
    }
}
