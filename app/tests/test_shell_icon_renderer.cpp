// The runtime brand renderer.
//
// Shell icons are the one part of the product a screenshot harness cannot see:
// they live outside our window, so QQuickWindow::grabWindow renders our scene
// graph and none of them. What CAN be asserted is the image itself, before it
// ever reaches the shell -- which is what this file does, on the pixels.

#include "models/RecordingPulse.h"
#include "ui/brand/BrandMark.h"
#include "ui/brand/BrandMarkSvg.h"
#include "ui/brand/ShellIconRenderer.h"
#include "ui/theme/ExoSnapThemes.h"

#include <QColor>
#include <QDir>
#include <QImage>
#include <QList>
#include <QPainter>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>

#include <cmath>
#include <cstring>
#include <iterator>

#include <gtest/gtest.h>

using exosnap::ShellIconState;
using exosnap::ui::brand::BrandMarkAspect;
using exosnap::ui::brand::BrandMarkAssetPath;
using exosnap::ui::brand::BrandMarkFrameCount;
using exosnap::ui::brand::BrandMarkIsAnimated;
using exosnap::ui::brand::BrandMarkKind;
using exosnap::ui::brand::BrandMarkKindFor;
using exosnap::ui::brand::BrandMarkPalette;
using exosnap::ui::brand::GlyphImageId;
using exosnap::ui::brand::kLargeProfile;
using exosnap::ui::brand::kMediumProfile;
using exosnap::ui::brand::kMediumProfileMaxPx;
using exosnap::ui::brand::kSmallProfile;
using exosnap::ui::brand::kSmallProfileMaxPx;
using exosnap::ui::brand::MarkImageId;
using exosnap::ui::brand::OpticalProfileFor;
using exosnap::ui::brand::ParseGlyphImageId;
using exosnap::ui::brand::ParseMarkImageId;
using exosnap::ui::brand::RenderGlyph;
using exosnap::ui::brand::RenderMark;
using exosnap::ui::brand::ResolveAccent;
using exosnap::ui::brand::ResolvePalette;
using exosnap::ui::brand::ShellGlyph;
using exosnap::ui::brand::ShellGlyphRequest;
using exosnap::ui::brand::ShellIconCache;
using exosnap::ui::brand::ShellIconImageUrl;
using exosnap::ui::brand::ShellMarkRequest;
using exosnap::ui::brand::ThemedBrandMarkSvg;

namespace {

// Every drawing the suite ships, so a sweep cannot silently skip one.
constexpr BrandMarkKind kAllKinds[] = {
    BrandMarkKind::Brand,  BrandMarkKind::Idle,  BrandMarkKind::Recording, BrandMarkKind::Processing,
    BrandMarkKind::Paused, BrandMarkKind::Saved, BrandMarkKind::Warning,   BrandMarkKind::Error,
};

constexpr ShellGlyph kAllGlyphs[] = {
    ShellGlyph::Record, ShellGlyph::Pause,         ShellGlyph::Resume, ShellGlyph::Stop,
    ShellGlyph::Window, ShellGlyph::Notifications, ShellGlyph::Folder, ShellGlyph::Quit,
};

QString KindName(BrandMarkKind kind) {
    return kind == BrandMarkKind::Recording ? QStringLiteral("recording") : QStringLiteral("processing");
}

ShellMarkRequest Mark(BrandMarkKind kind, int px = 64) {
    ShellMarkRequest request;
    request.kind = kind;
    request.px = px;
    request.appearance_id = QStringLiteral("dark");
    request.accent_id = QStringLiteral("aqua");
    return request;
}

// The average colour of an annulus, as a fraction of the image's side. Sampled
// rather than read from one pixel, because an antialiased ring has no pixel that
// is reliably fully on it.
QColor RingAverage(const QImage& image, double inner_fraction, double outer_fraction) {
    const double side = image.width();
    const double centre = side / 2.0;
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double weight = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const double dx = x + 0.5 - centre;
            const double dy = y + 0.5 - centre;
            const double distance = std::sqrt(dx * dx + dy * dy) / side;
            if (distance < inner_fraction || distance > outer_fraction)
                continue;
            const QColor pixel = image.pixelColor(x, y);
            const double alpha = pixel.alphaF();
            r += pixel.redF() * alpha;
            g += pixel.greenF() * alpha;
            b += pixel.blueF() * alpha;
            weight += alpha;
        }
    }
    if (weight <= 0.0)
        return QColor(Qt::transparent);
    return QColor::fromRgbF(static_cast<float>(r / weight), static_cast<float>(g / weight),
                            static_cast<float>(b / weight));
}

// The centre dot, and the inner ring with whatever glyph sits inside it. The
// bands are wide enough to survive the optical profiles moving the drawing
// slightly and narrow enough to exclude the outer ring, which is the accent in
// every state.
QColor DotAverage(const QImage& image) {
    return RingAverage(image, 0.0, 0.09);
}

QColor InnerAverage(const QImage& image) {
    return RingAverage(image, 0.14, 0.30);
}

// The average colour of the covered pixels in a horizontal band, as fractions of
// the image width. Alpha-weighted, because glyph edges are antialiased against
// nothing and a plain mean would drag every colour towards black.
QColor AverageInk(const QImage& image, double from_fraction, double to_fraction) {
    const int from = qBound(0, qRound(from_fraction * image.width()), image.width());
    const int to = qBound(from, qRound(to_fraction * image.width()), image.width());
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double weight = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = from; x < to; ++x) {
            const QColor pixel = image.pixelColor(x, y);
            const double alpha = pixel.alphaF();
            r += pixel.redF() * alpha;
            g += pixel.greenF() * alpha;
            b += pixel.blueF() * alpha;
            weight += alpha;
        }
    }
    if (weight <= 0.0)
        return QColor(Qt::transparent);
    return QColor::fromRgbF(static_cast<float>(r / weight), static_cast<float>(g / weight),
                            static_cast<float>(b / weight));
}

double CoveredAlpha(const QImage& image) {
    double total = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x)
            total += image.pixelColor(x, y).alphaF();
    }
    return total;
}

int HueDistance(const QColor& lhs, const QColor& rhs) {
    const int a = lhs.hue();
    const int b = rhs.hue();
    if (a < 0 || b < 0)
        return 360;
    const int raw = std::abs(a - b);
    return std::min(raw, 360 - raw);
}

double StrokeWidthIn(const QByteArray& svg) {
    const QString text = QString::fromUtf8(svg);
    const int start = text.indexOf(QStringLiteral("stroke-width=\""));
    if (start < 0)
        return 0.0;
    const int from = start + int(std::strlen("stroke-width=\""));
    return text.mid(from, text.indexOf(QLatin1Char('"'), from) - from).toDouble();
}

double OuterOpacityIn(const QByteArray& svg) {
    const QString text = QString::fromUtf8(svg);
    const int start = text.indexOf(QStringLiteral("opacity=\""));
    if (start < 0)
        return 0.0;
    const int from = start + int(std::strlen("opacity=\""));
    return text.mid(from, text.indexOf(QLatin1Char('"'), from) - from).toDouble();
}

} // namespace

// -- the suite ---------------------------------------------------------------

TEST(ShellIconRenderer, EveryMarkInTheInventoryRenders) {
    // The resource is compiled into this binary, so a missing or misnamed asset
    // fails here rather than as an empty tray icon on a user's machine.
    for (const BrandMarkKind kind : kAllKinds) {
        const QImage image = RenderMark(Mark(kind));
        EXPECT_FALSE(image.isNull()) << BrandMarkAssetPath(kind).toStdString() << " did not render";
        EXPECT_GT(CoveredAlpha(image), 0.0) << BrandMarkAssetPath(kind).toStdString() << " rendered empty";
    }
}

TEST(ShellIconRenderer, EveryMarkIsVisiblyItsOwnDrawing) {
    QList<QImage> rendered;
    for (const BrandMarkKind kind : kAllKinds) {
        if (kind == BrandMarkKind::Brand)
            continue; // Brand and Idle are deliberately the same drawing.
        rendered.append(RenderMark(Mark(kind)));
    }
    for (int i = 0; i < rendered.size(); ++i) {
        for (int j = i + 1; j < rendered.size(); ++j)
            EXPECT_NE(rendered.at(i), rendered.at(j)) << "marks " << i << " and " << j << " are the same image";
    }
}

TEST(ShellIconRenderer, IdleIsTheBrandMark) {
    // Two names for one drawing: the product's identity, and the state that has
    // nothing else to say. If they ever diverge it is a design decision, and this
    // is where it gets noticed.
    EXPECT_EQ(RenderMark(Mark(BrandMarkKind::Idle)), RenderMark(Mark(BrandMarkKind::Brand)));
}

// -- determinism -------------------------------------------------------------

TEST(ShellIconRenderer, TheSameRequestProducesTheSameImage) {
    EXPECT_EQ(RenderMark(Mark(BrandMarkKind::Recording)), RenderMark(Mark(BrandMarkKind::Recording)));
}

TEST(ShellIconRenderer, TheApertureMarksAreSquare) {
    // The renderer takes its aspect from the asset now. Every drawing in the
    // aperture suite is authored on a square grid, so this is the half of that
    // change nothing may notice.
    for (const BrandMarkKind kind : kAllKinds) {
        const QImage image = RenderMark(Mark(kind, 40));
        EXPECT_EQ(image.width(), 40) << BrandMarkAssetPath(kind).toStdString();
        EXPECT_EQ(image.height(), 40) << BrandMarkAssetPath(kind).toStdString();
    }
}

TEST(ShellIconRenderer, TheWordmarkRendersAtTheAspectItsViewBoxAsks) {
    // `px` is the raster HEIGHT. A wordmark forced into a square would either be
    // letterboxed to a fifth of the height it asked for or squeezed flat, and
    // both look like a bug in the title band rather than a bug in the renderer.
    const QImage image = RenderMark(Mark(BrandMarkKind::Wordmark, 32));
    ASSERT_FALSE(image.isNull());
    EXPECT_EQ(image.height(), 32);
    EXPECT_EQ(image.width(), qRound(32.0 * BrandMarkAspect(BrandMarkKind::Wordmark)));
    EXPECT_GT(image.width(), 3 * image.height());
    EXPECT_GT(CoveredAlpha(image), 0.0);
}

TEST(ShellIconRenderer, TheWordmarkReadsAsInkThenAccent) {
    // The two halves of the product name carry different roles, and a
    // substitution that missed the ink literal would leave `exo` in the
    // designer's near-white on a light appearance -- invisible, and nothing else
    // would catch it.
    const QImage image = RenderMark(Mark(BrandMarkKind::Wordmark, 64));
    ASSERT_FALSE(image.isNull());

    const QColor exo = AverageInk(image, 0.0, 0.40);
    const QColor snap = AverageInk(image, 0.48, 1.0);
    const BrandMarkPalette palette = ResolvePalette(QStringLiteral("dark"), QStringLiteral("aqua"));

    EXPECT_LT(HueDistance(snap, palette.accent), 20) << "`snap` is not the accent";
    // Ink is a near-neutral, so it is asserted by saturation rather than by hue.
    EXPECT_LT(exo.saturation(), 40) << "`exo` is not drawn in the appearance's ink";
    EXPECT_GT(exo.lightness(), 200) << "`exo` is not the dark appearance's near-white ink";
}

TEST(ShellIconRenderer, TheWordmarkInkFollowsTheAppearance) {
    // The whole reason the wordmark stopped being two Labels is that a raster
    // must follow the theme the labels did.
    ShellMarkRequest request = Mark(BrandMarkKind::Wordmark, 64);
    const QColor dark = AverageInk(RenderMark(request), 0.0, 0.40);
    request.appearance_id = QStringLiteral("light");
    const QColor light = AverageInk(RenderMark(request), 0.0, 0.40);
    EXPECT_GT(dark.lightness(), 200);
    EXPECT_LT(light.lightness(), 80);
}

TEST(ShellIconRenderer, TheRequestedSizeIsTheImageSize) {
    for (const int px : {16, 20, 24, 32, 48, 64, 256}) {
        const QImage image = RenderMark(Mark(BrandMarkKind::Idle, px));
        EXPECT_EQ(image.width(), px);
        EXPECT_EQ(image.height(), px);
    }
}

TEST(ShellIconRenderer, TheBackgroundIsTransparent) {
    const QImage image = RenderMark(Mark(BrandMarkKind::Idle, 32));
    EXPECT_EQ(image.pixelColor(0, 0).alpha(), 0);
    EXPECT_EQ(image.pixelColor(image.width() - 1, 0).alpha(), 0);
    EXPECT_EQ(image.pixelColor(0, image.height() - 1).alpha(), 0);
    EXPECT_EQ(image.pixelColor(image.width() - 1, image.height() - 1).alpha(), 0);
    EXPECT_GT(CoveredAlpha(image), 0.0);
}

// -- state semantics ---------------------------------------------------------

TEST(ShellIconRenderer, EachMarkCarriesItsOwnSemanticColourInside) {
    const auto& dark = exosnap::ui::theme::kExoAppearances.front();
    ASSERT_STREQ(dark.id, "dark");

    EXPECT_LE(
        HueDistance(InnerAverage(RenderMark(Mark(BrandMarkKind::Recording))), QColor(QString::fromLatin1(dark.error))),
        12);
    EXPECT_LE(
        HueDistance(InnerAverage(RenderMark(Mark(BrandMarkKind::Paused))), QColor(QString::fromLatin1(dark.caution))),
        12);
    EXPECT_LE(
        HueDistance(InnerAverage(RenderMark(Mark(BrandMarkKind::Warning))), QColor(QString::fromLatin1(dark.caution))),
        12);
    EXPECT_LE(
        HueDistance(InnerAverage(RenderMark(Mark(BrandMarkKind::Saved))), QColor(QString::fromLatin1(dark.success))),
        12);
    EXPECT_LE(
        HueDistance(InnerAverage(RenderMark(Mark(BrandMarkKind::Error))), QColor(QString::fromLatin1(dark.error))), 12);
}

TEST(ShellIconRenderer, TheBrandMarkIsTheAccentWithTheRecordingDot) {
    const QImage image = RenderMark(Mark(BrandMarkKind::Idle));
    const auto& dark = exosnap::ui::theme::kExoAppearances.front();
    EXPECT_LE(HueDistance(InnerAverage(image), ResolveAccent(QStringLiteral("dark"), QStringLiteral("aqua"))), 12);
    // The dot is the recording colour even at rest: the aperture is trained on
    // something, and that is what makes a running recording read as a change of
    // weight rather than as a different logo.
    EXPECT_LE(HueDistance(DotAverage(image), QColor(QString::fromLatin1(dark.error))), 12);
}

TEST(ShellIconRenderer, AnAccentChangeMovesTheBrandButNotTheState) {
    ShellMarkRequest aqua = Mark(BrandMarkKind::Recording);
    ShellMarkRequest magenta = aqua;
    magenta.accent_id = QStringLiteral("magenta");

    // The outer ring is the brand and follows the accent, so the images differ.
    EXPECT_NE(RenderMark(aqua), RenderMark(magenta));
    // The inner ring is the SESSION and must not: a recording is coral whatever
    // the user picked, or the accent becomes a second state channel.
    EXPECT_LE(HueDistance(InnerAverage(RenderMark(aqua)), InnerAverage(RenderMark(magenta))), 3);
}

TEST(ShellIconRenderer, AnAppearanceChangeResolvesTheAccentForThatAppearance) {
    // Aqua is #9BD9D2 on Dark and #127C74 on Light -- the same hue at a very
    // different lightness, because a value that reads on near-black does not on
    // near-white.
    const QColor dark = ResolveAccent(QStringLiteral("dark"), QStringLiteral("aqua"));
    const QColor light = ResolveAccent(QStringLiteral("light"), QStringLiteral("aqua"));
    EXPECT_NE(dark, light);
    EXPECT_LE(HueDistance(dark, light), 12);
}

TEST(ShellIconRenderer, TheLightAppearanceCarriesTheHeavierOuterRing) {
    // The one difference between the two themes' marks. A pale ring that reads
    // on near-black disappears on near-white, and the suite is authored at the
    // dark value.
    const auto& profile = kLargeProfile;
    const double dark = OuterOpacityIn(ThemedBrandMarkSvg(
        BrandMarkKind::Idle, 0, ResolvePalette(QStringLiteral("dark"), QStringLiteral("aqua")), profile));
    const double light = OuterOpacityIn(ThemedBrandMarkSvg(
        BrandMarkKind::Idle, 0, ResolvePalette(QStringLiteral("light"), QStringLiteral("aqua")), profile));
    EXPECT_DOUBLE_EQ(dark, exosnap::ui::brand::kOuterOpacityDark);
    EXPECT_DOUBLE_EQ(light, exosnap::ui::brand::kOuterOpacityLight);
    EXPECT_GT(light, dark);
}

TEST(ShellIconRenderer, AnUnknownPaletteIdFallsBackToTheShippedDefault) {
    EXPECT_EQ(ResolveAccent(QStringLiteral("from-a-build-that-never-existed"), QStringLiteral("chartreuse")),
              ResolveAccent(QStringLiteral("dark"), QStringLiteral("aqua")));
    EXPECT_EQ(ResolvePalette(QStringLiteral("nonsense"), QStringLiteral("nonsense")).recording,
              ResolvePalette(QStringLiteral("dark"), QStringLiteral("aqua")).recording);
}

TEST(ShellIconRenderer, TheShellStatesMapOntoTheDrawingsThatExist) {
    EXPECT_EQ(BrandMarkKindFor(ShellIconState::Idle), BrandMarkKind::Idle);
    EXPECT_EQ(BrandMarkKindFor(ShellIconState::Recording), BrandMarkKind::Recording);
    EXPECT_EQ(BrandMarkKindFor(ShellIconState::Processing), BrandMarkKind::Processing);
    EXPECT_EQ(BrandMarkKindFor(ShellIconState::Paused), BrandMarkKind::Paused);
    EXPECT_EQ(BrandMarkKindFor(ShellIconState::Saved), BrandMarkKind::Saved);
    EXPECT_EQ(BrandMarkKindFor(ShellIconState::Error), BrandMarkKind::Error);
}

// -- the animations ----------------------------------------------------------

TEST(ShellIconRendererAnimation, OnlyTheTwoSequencesHaveFrames) {
    for (const BrandMarkKind kind : kAllKinds) {
        const bool animated = kind == BrandMarkKind::Recording || kind == BrandMarkKind::Processing;
        EXPECT_EQ(BrandMarkIsAnimated(kind), animated);
        EXPECT_EQ(BrandMarkFrameCount(kind) > 1, animated);
    }
}

TEST(ShellIconRendererAnimation, ASequenceMovesOnEveryTickExceptItsRest) {
    // The recording loop's last frame repeats its first on purpose -- that pause
    // at the bottom is what makes it a heartbeat. Every other pair differs, or
    // the sequence would be holding still where it should be moving.
    for (const BrandMarkKind kind : {BrandMarkKind::Recording, BrandMarkKind::Processing}) {
        QList<QImage> frames;
        for (int frame = 0; frame < BrandMarkFrameCount(kind); ++frame) {
            ShellMarkRequest request = Mark(kind);
            request.frame = frame;
            const QImage image = RenderMark(request);
            ASSERT_FALSE(image.isNull()) << BrandMarkAssetPath(kind, frame).toStdString() << " did not render";
            frames.append(image);
        }
        const int last = int(frames.size()) - 1;
        for (int i = 0; i < frames.size(); ++i) {
            for (int j = i + 1; j < frames.size(); ++j) {
                if (kind == BrandMarkKind::Recording && i == 0 && j == last)
                    continue;
                EXPECT_NE(frames.at(i), frames.at(j)) << "frames " << i << " and " << j << " are identical";
            }
        }
    }
}

TEST(ShellIconRendererAnimation, TheBeatIsBrightnessAndNothingElse) {
    // What makes a permanent beat affordable at 16 px. A frame that moved a
    // radius as well would differ from its neighbour by well under a device
    // pixel, and that reads as a flicker rather than as a heartbeat.
    const auto palette = ResolvePalette(QStringLiteral("dark"), QStringLiteral("aqua"));
    const QRegularExpression radius(QStringLiteral("<circle[^>]*r=\"([0-9.]+)\"[^>]*stroke-width"));
    QSet<QString> inner_radii;
    QSet<QString> opacities;
    for (int frame = 0; frame < BrandMarkFrameCount(BrandMarkKind::Recording); ++frame) {
        const QString svg =
            QString::fromUtf8(ThemedBrandMarkSvg(BrandMarkKind::Recording, frame, palette, kLargeProfile));
        QRegularExpressionMatchIterator it = radius.globalMatch(svg);
        while (it.hasNext())
            inner_radii.insert(it.next().captured(1));
        opacities.insert(svg);
    }
    // Two radii across the whole loop: the outer ring and the inner one, neither
    // of which moves.
    EXPECT_EQ(inner_radii.size(), 2);
    // And the frames are nonetheless different, which is the brightness. One
    // fewer than the frame count, because the loop's rest repeats its first
    // frame.
    EXPECT_EQ(opacities.size(), BrandMarkFrameCount(BrandMarkKind::Recording) - 1);
}

TEST(ShellIconRendererAnimation, TheLoopRestsAtItsDimmestFrame) {
    // The first and last frames are the same drawing on purpose: the beat pauses
    // at the bottom, which is what makes it a heartbeat rather than a metronome.
    const int last = BrandMarkFrameCount(BrandMarkKind::Recording) - 1;
    ShellMarkRequest first = Mark(BrandMarkKind::Recording, 32);
    ShellMarkRequest rest = first;
    rest.frame = last;
    EXPECT_EQ(RenderMark(first), RenderMark(rest));

    // And the middle of the loop is brighter than either end.
    ShellMarkRequest peak = first;
    peak.frame = 2;
    EXPECT_GT(CoveredAlpha(RenderMark(peak)), CoveredAlpha(RenderMark(first)));
}

TEST(ShellIconRendererAnimation, AFrameCounterThatRanPastTheEndStillNamesAFrame) {
    // The counter indexes a beat, not an array. Wrapping rather than clamping is
    // what keeps a tick delivered late from asking for a resource that is not
    // there.
    EXPECT_EQ(BrandMarkAssetPath(BrandMarkKind::Recording, BrandMarkFrameCount(BrandMarkKind::Recording)),
              BrandMarkAssetPath(BrandMarkKind::Recording, 0));
    EXPECT_EQ(BrandMarkAssetPath(BrandMarkKind::Recording, -1),
              BrandMarkAssetPath(BrandMarkKind::Recording, BrandMarkFrameCount(BrandMarkKind::Recording) - 1));
}

TEST(ShellIconRendererAnimation, AStaticMarkIgnoresTheFrame) {
    EXPECT_EQ(BrandMarkAssetPath(BrandMarkKind::Paused, 3), BrandMarkAssetPath(BrandMarkKind::Paused, 0));
}

// -- optical profiles --------------------------------------------------------

TEST(ShellIconRendererOpticalSizing, TheProfileBoundariesAreDeterministic) {
    EXPECT_EQ(&OpticalProfileFor(kSmallProfileMaxPx), &kSmallProfile);
    EXPECT_EQ(&OpticalProfileFor(kSmallProfileMaxPx + 1), &kMediumProfile);
    EXPECT_EQ(&OpticalProfileFor(kMediumProfileMaxPx), &kMediumProfile);
    EXPECT_EQ(&OpticalProfileFor(kMediumProfileMaxPx + 1), &kLargeProfile);
    // Total, including nonsense: a size of zero still resolves to a profile.
    EXPECT_EQ(&OpticalProfileFor(0), &kSmallProfile);
    EXPECT_EQ(&OpticalProfileFor(-1), &kSmallProfile);
}

TEST(ShellIconRendererOpticalSizing, SmallSizesAreCorrectedAndLargeOnesAreNot) {
    EXPECT_GT(kSmallProfile.ring_stroke_scale, kMediumProfile.ring_stroke_scale);
    EXPECT_GT(kMediumProfile.ring_stroke_scale, kLargeProfile.ring_stroke_scale);
    EXPECT_GT(kSmallProfile.outer_opacity_scale, kLargeProfile.outer_opacity_scale);
    EXPECT_DOUBLE_EQ(kLargeProfile.ring_stroke_scale, 1.0);
    EXPECT_DOUBLE_EQ(kLargeProfile.outer_opacity_scale, 1.0);
    EXPECT_DOUBLE_EQ(kLargeProfile.content_scale, 1.0);
}

TEST(ShellIconRendererOpticalSizing, TheCorrectionReachesTheDrawingAndNotOnlyTheProfile) {
    const auto palette = ResolvePalette(QStringLiteral("dark"), QStringLiteral("aqua"));
    const double large = StrokeWidthIn(ThemedBrandMarkSvg(BrandMarkKind::Idle, 0, palette, kLargeProfile));
    const double small = StrokeWidthIn(ThemedBrandMarkSvg(BrandMarkKind::Idle, 0, palette, kSmallProfile));
    ASSERT_GT(large, 0.0);
    EXPECT_GT(small, large);
    EXPECT_NEAR(small, large * kSmallProfile.ring_stroke_scale, 1e-6);
}

TEST(ShellIconRendererOpticalSizing, TheGlyphsAreLeftAtTheirAuthoredWeight) {
    // The correction reaches the rings and stops there. Thickening the glyph as
    // well grew it against an aperture that had not moved, and at 16 px the void
    // inside the inner ring closed up entirely.
    const auto palette = ResolvePalette(QStringLiteral("dark"), QStringLiteral("aqua"));
    const QString large = QString::fromUtf8(ThemedBrandMarkSvg(BrandMarkKind::Saved, 0, palette, kLargeProfile));
    const QString small = QString::fromUtf8(ThemedBrandMarkSvg(BrandMarkKind::Saved, 0, palette, kSmallProfile));

    // The check is a <path>, and its stroke is the same in both.
    const QRegularExpression check(QStringLiteral("<path[^>]*stroke-width=\"([0-9.]+)\""));
    EXPECT_DOUBLE_EQ(check.match(small).captured(1).toDouble(), check.match(large).captured(1).toDouble());

    // The pause bars are fills, and they are untouched for the same reason.
    const QString bars_large = QString::fromUtf8(ThemedBrandMarkSvg(BrandMarkKind::Paused, 0, palette, kLargeProfile));
    const QString bars_small = QString::fromUtf8(ThemedBrandMarkSvg(BrandMarkKind::Paused, 0, palette, kSmallProfile));
    const QRegularExpression bar(QStringLiteral("<rect[^>]*width=\"([0-9.]+)\""));
    EXPECT_DOUBLE_EQ(bar.match(bars_small).captured(1).toDouble(), bar.match(bars_large).captured(1).toDouble());
}

TEST(ShellIconRendererOpticalSizing, ASmallMarkKeepsARealGapBetweenTheDotAndTheInnerRing) {
    // At 16 px there are eight device pixels from the centre to the edge, and all
    // three elements plus two gaps have to fit in them. One column of partial
    // coverage does not read as a hole: the dot and the ring would merge into a
    // blob and the mark would stop being an aperture.
    //
    // Measured along the centre row, walking outwards from the dot.
    constexpr double kInk = 0.30;
    constexpr int kRequiredGapColumns = 2;

    const QImage image = RenderMark(Mark(BrandMarkKind::Idle, 16));
    const int mid = image.height() / 2;
    int x = image.width() / 2 - 1;

    ASSERT_GT(image.pixelColor(x, mid).alphaF(), kInk) << "no centre dot";
    while (x >= 0 && image.pixelColor(x, mid).alphaF() > kInk)
        --x;

    int gap = 0;
    while (x >= 0 && image.pixelColor(x, mid).alphaF() <= kInk) {
        ++gap;
        --x;
    }
    EXPECT_GE(gap, kRequiredGapColumns) << "only " << gap << " column(s) between the dot and the inner ring at 16 px";
    ASSERT_GE(x, 0) << "no inner ring outside the dot";
    EXPECT_GT(image.pixelColor(x, mid).alphaF(), 0.5) << "the inner ring is too faint to read at 16 px";
}

TEST(ShellIconRendererOpticalSizing, AShellIconReservesMarginAndAnInlineOneDoesNot) {
    ShellMarkRequest standalone = Mark(BrandMarkKind::Idle, 64);
    ShellMarkRequest inline_mark = standalone;
    inline_mark.standalone = false;
    // The inline mark fills its box, so it draws more ink at the same size. A
    // mark placed by a layout that reserved a tray icon's margin reads as too
    // small for the space it was given.
    EXPECT_GT(CoveredAlpha(RenderMark(inline_mark)), CoveredAlpha(RenderMark(standalone)));
}

// -- glyphs ------------------------------------------------------------------

TEST(ShellIconRendererGlyphs, EveryMenuShapeIsItsOwn) {
    // Every row of the tray menu carries one. A menu where three rows have an
    // icon and four do not reads as three unfinished rows.
    QList<QImage> rendered;
    for (const ShellGlyph glyph : kAllGlyphs) {
        ShellGlyphRequest request;
        request.glyph = glyph;
        request.px = 32;
        request.appearance_id = QStringLiteral("dark");
        request.accent_id = QStringLiteral("aqua");
        const QImage image = RenderGlyph(request);
        EXPECT_EQ(image.pixelColor(0, 0).alpha(), 0);
        EXPECT_GT(CoveredAlpha(image), 0.0) << "glyph " << static_cast<int>(glyph) << " drew nothing";
        rendered.append(image);
    }
    for (int i = 0; i < rendered.size(); ++i) {
        for (int j = i + 1; j < rendered.size(); ++j)
            EXPECT_NE(rendered.at(i), rendered.at(j)) << "glyphs " << i << " and " << j << " are the same image";
    }
}

TEST(ShellIconRendererGlyphs, EveryGlyphIdRoundTrips) {
    // The menu addresses them by URL, so a token that parses back to a different
    // glyph is a row with the wrong icon and nothing to notice it.
    for (const ShellGlyph glyph : kAllGlyphs) {
        ShellGlyphRequest request;
        request.glyph = glyph;
        request.px = 16;
        request.appearance_id = QStringLiteral("dark");
        request.accent_id = QStringLiteral("aqua");
        ShellGlyphRequest parsed;
        ASSERT_TRUE(ParseGlyphImageId(GlyphImageId(request), parsed)) << GlyphImageId(request).toStdString();
        EXPECT_EQ(parsed.glyph, glyph) << GlyphImageId(request).toStdString();
    }
}

// -- image ids ---------------------------------------------------------------

TEST(ShellIconRendererIds, AMarkIdRoundTrips) {
    ShellMarkRequest request = Mark(BrandMarkKind::Recording, 24);
    request.frame = 3;
    request.standalone = false;
    request.accent_id = QStringLiteral("sky");

    ShellMarkRequest parsed;
    ASSERT_TRUE(ParseMarkImageId(MarkImageId(request), parsed));
    EXPECT_EQ(parsed.kind, request.kind);
    EXPECT_EQ(parsed.px, request.px);
    EXPECT_EQ(parsed.frame, request.frame);
    EXPECT_EQ(parsed.standalone, request.standalone);
    EXPECT_EQ(parsed.appearance_id, request.appearance_id);
    EXPECT_EQ(parsed.accent_id, request.accent_id);
}

TEST(ShellIconRendererIds, AStaticMarkDropsTheFrameFromTheId) {
    // Otherwise the same paused icon would have four URLs, and Qt Quick's pixmap
    // cache would hold four copies of one image.
    ShellMarkRequest a = Mark(BrandMarkKind::Paused);
    ShellMarkRequest b = a;
    b.frame = 3;
    EXPECT_EQ(MarkImageId(a), MarkImageId(b));
}

TEST(ShellIconRendererIds, TheMarginIsPartOfTheKey) {
    // The same mark at the same size is two different images depending on it, and
    // one URL that renders two ways is a stale icon nothing invalidates.
    ShellMarkRequest standalone = Mark(BrandMarkKind::Idle);
    ShellMarkRequest inline_mark = standalone;
    inline_mark.standalone = false;
    EXPECT_NE(MarkImageId(standalone), MarkImageId(inline_mark));
}

TEST(ShellIconRendererIds, AGlyphIdRoundTrips) {
    ShellGlyphRequest request;
    request.glyph = ShellGlyph::Resume;
    request.px = 20;
    request.appearance_id = QStringLiteral("light");
    request.accent_id = QStringLiteral("magenta");

    ShellGlyphRequest parsed;
    ASSERT_TRUE(ParseGlyphImageId(GlyphImageId(request), parsed));
    EXPECT_EQ(parsed.glyph, request.glyph);
    EXPECT_EQ(parsed.px, request.px);
    EXPECT_EQ(parsed.appearance_id, request.appearance_id);
    EXPECT_EQ(parsed.accent_id, request.accent_id);
}

TEST(ShellIconRendererIds, MalformedIdsAreRefusedRatherThanGuessed) {
    ShellMarkRequest mark;
    ShellGlyphRequest glyph;
    EXPECT_FALSE(ParseMarkImageId(QStringLiteral("mark/recording/32/0/shell/dark"), mark));
    EXPECT_FALSE(ParseMarkImageId(QStringLiteral("mark/spinning/32/0/shell/dark/aqua"), mark));
    EXPECT_FALSE(ParseMarkImageId(QStringLiteral("mark/recording/big/0/shell/dark/aqua"), mark));
    EXPECT_FALSE(ParseMarkImageId(QStringLiteral("mark/recording/32/0/floating/dark/aqua"), mark));
    EXPECT_FALSE(ParseMarkImageId(QStringLiteral("glyph/pause/16/dark/aqua"), mark));
    EXPECT_FALSE(ParseGlyphImageId(QStringLiteral("glyph/rewind/16/dark/aqua"), glyph));
}

TEST(ShellIconRendererIds, TheUrlAddressesTheProvider) {
    EXPECT_EQ(ShellIconImageUrl(QStringLiteral("mark/idle/16/0/shell/dark/aqua")),
              QStringLiteral("image://exosnap-shell/mark/idle/16/0/shell/dark/aqua"));
}

// -- the cache ---------------------------------------------------------------

TEST(ShellIconRendererCache, OneEntryPerDistinctKey) {
    ShellIconCache cache;
    const ShellMarkRequest recording = Mark(BrandMarkKind::Recording);

    (void)cache.mark(recording);
    (void)cache.mark(recording);
    EXPECT_EQ(cache.sizeForTest(), 1);

    ShellMarkRequest bigger = recording;
    bigger.px = 24;
    (void)cache.mark(bigger);
    EXPECT_EQ(cache.sizeForTest(), 2);

    ShellMarkRequest other_accent = recording;
    other_accent.accent_id = QStringLiteral("sky");
    (void)cache.mark(other_accent);
    EXPECT_EQ(cache.sizeForTest(), 3);
}

TEST(ShellIconRendererCache, AWholeRecordingCostsTheFramesAndNoMore) {
    // The beat is a transition, so a recording of any length is a fixed handful
    // of rasters. A cache that grew with the recording would be an allocation on
    // a timer for as long as the session lasts.
    ShellIconCache cache;
    for (int tick = 0; tick < 200; ++tick) {
        ShellMarkRequest request = Mark(BrandMarkKind::Recording, 16);
        request.frame = tick % BrandMarkFrameCount(BrandMarkKind::Recording);
        (void)cache.mark(request);
    }
    EXPECT_EQ(cache.sizeForTest(), BrandMarkFrameCount(BrandMarkKind::Recording));
}

TEST(ShellIconRendererCache, ACachedImageIsTheRenderedOne) {
    ShellIconCache cache;
    const ShellMarkRequest request = Mark(BrandMarkKind::Saved, 20);
    EXPECT_EQ(cache.mark(request), RenderMark(request));
    EXPECT_EQ(cache.mark(request), RenderMark(request));
}

TEST(ShellIconRendererCache, NoThemeVariantCanGoStale) {
    // The palette ids are part of the key, so a changed accent cannot be served
    // the previous accent's raster -- which is the one cache bug a tray icon
    // would show for the rest of the session.
    ShellIconCache cache;
    ShellMarkRequest aqua = Mark(BrandMarkKind::Idle);
    ShellMarkRequest magenta = aqua;
    magenta.accent_id = QStringLiteral("magenta");

    const QImage first = cache.mark(aqua);
    const QImage second = cache.mark(magenta);
    EXPECT_NE(first, second);
    EXPECT_EQ(cache.mark(aqua), first);
}

TEST(ShellIconRendererCache, ClearEmptiesIt) {
    ShellIconCache cache;
    (void)cache.mark(Mark(BrandMarkKind::Idle));
    ASSERT_GT(cache.sizeForTest(), 0);
    cache.clear();
    EXPECT_EQ(cache.sizeForTest(), 0);
}

// -- optical evidence --------------------------------------------------------

TEST(ShellIconRendererEvidence, WritesTheSizeSweepWhenAskedFor) {
    // The optical profiles are chosen from rendered evidence rather than derived,
    // so the evidence has to be reproducible: set EXOSNAP_SHELL_ICON_EVIDENCE_DIR
    // and this writes one sheet per appearance, every mark at every shell size,
    // exactly as the notification area would receive them.
    //
    // Off by default. It writes files, and a test that writes files on every run
    // is a test that fails on a read-only checkout.
    const QString directory =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("EXOSNAP_SHELL_ICON_EVIDENCE_DIR"));
    if (directory.isEmpty())
        GTEST_SKIP() << "set EXOSNAP_SHELL_ICON_EVIDENCE_DIR to write the sweep";
    ASSERT_TRUE(QDir().mkpath(directory)) << directory.toStdString();

    const QList<int> sizes{16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
    constexpr int kCell = 272;
    constexpr int kMargin = 8;

    for (const QString& appearance : {QStringLiteral("dark"), QStringLiteral("light")}) {
        QImage sheet(kMargin + int(sizes.size()) * kCell, kMargin + std::size(kAllKinds) * kCell,
                     QImage::Format_ARGB32_Premultiplied);
        sheet.fill(appearance == QStringLiteral("dark") ? QColor(0x0E, 0x0E, 0x10) : QColor(0xE7, 0xE9, 0xED));
        QPainter painter(&sheet);
        int row = 0;
        for (const BrandMarkKind kind : kAllKinds) {
            int column = 0;
            for (const int px : sizes) {
                ShellMarkRequest request = Mark(kind, px);
                request.appearance_id = appearance;
                const QImage image = RenderMark(request);
                painter.drawImage(kMargin + column * kCell, kMargin + row * kCell, image);
                // The same raster again, magnified with no smoothing, because
                // what a 16 px icon does is only visible per pixel.
                painter.drawImage(QRect(kMargin + column * kCell, kMargin + row * kCell + 40, 224, 224), image);
                ++column;
            }
            ++row;
        }
        painter.end();
        const QString path = QStringLiteral("%1/shell-icon-sweep-%2.png").arg(directory, appearance);
        EXPECT_TRUE(sheet.save(path)) << path.toStdString();

        // The menu glyphs, at the sizes a menu row actually draws one.
        const QList<int> glyph_sizes{16, 20, 24, 32};
        QImage strip(kMargin + int(glyph_sizes.size()) * kCell, kMargin + std::size(kAllGlyphs) * kCell,
                     QImage::Format_ARGB32_Premultiplied);
        strip.fill(appearance == QStringLiteral("dark") ? QColor(0x0E, 0x0E, 0x10) : QColor(0xE7, 0xE9, 0xED));
        QPainter glyph_painter(&strip);
        int glyph_row = 0;
        for (const ShellGlyph glyph : kAllGlyphs) {
            int column = 0;
            for (const int px : glyph_sizes) {
                ShellGlyphRequest request;
                request.glyph = glyph;
                request.px = px;
                request.appearance_id = appearance;
                request.accent_id = QStringLiteral("aqua");
                const QImage image = RenderGlyph(request);
                glyph_painter.drawImage(QRect(kMargin + column * kCell, kMargin + glyph_row * kCell, 224, 224), image);
                ++column;
            }
            ++glyph_row;
        }
        glyph_painter.end();
        const QString glyph_path = QStringLiteral("%1/menu-glyphs-%2.png").arg(directory, appearance);
        EXPECT_TRUE(strip.save(glyph_path)) << glyph_path.toStdString();

        // The two loops, frame by frame, at the sizes the shell plays them at.
        for (const BrandMarkKind kind : {BrandMarkKind::Recording, BrandMarkKind::Processing}) {
            const int count = BrandMarkFrameCount(kind);
            QImage loop(kMargin + count * kCell, kMargin + int(glyph_sizes.size()) * kCell,
                        QImage::Format_ARGB32_Premultiplied);
            loop.fill(appearance == QStringLiteral("dark") ? QColor(0x0E, 0x0E, 0x10) : QColor(0xE7, 0xE9, 0xED));
            QPainter loop_painter(&loop);
            int size_row = 0;
            for (const int px : glyph_sizes) {
                for (int frame = 0; frame < count; ++frame) {
                    ShellMarkRequest request = Mark(kind, px);
                    request.frame = frame;
                    request.appearance_id = appearance;
                    loop_painter.drawImage(QRect(kMargin + frame * kCell, kMargin + size_row * kCell, 224, 224),
                                           RenderMark(request));
                }
                ++size_row;
            }
            loop_painter.end();
            const QString loop_path = QStringLiteral("%1/loop-%2-%3.png").arg(directory, KindName(kind), appearance);
            EXPECT_TRUE(loop.save(loop_path)) << loop_path.toStdString();
        }
    }
}
