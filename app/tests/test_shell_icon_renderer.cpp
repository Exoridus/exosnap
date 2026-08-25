// The runtime brand renderer.
//
// Shell icons are the one part of the product a screenshot harness cannot see:
// they live outside our window, so QQuickWindow::grabWindow renders our scene
// graph and none of them. What CAN be asserted is the image itself, before it
// ever reaches the shell -- which is what this file does, on the pixels.

#include "models/RecordingPulse.h"
#include "ui/brand/BrandMark.h"
#include "ui/brand/ShellIconRenderer.h"
#include "ui/theme/ExoSnapThemes.h"

#include <QColor>
#include <QImage>

#include <gtest/gtest.h>

using exosnap::ShellIconState;
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
using exosnap::ui::brand::PulseOpacity;
using exosnap::ui::brand::RenderGlyph;
using exosnap::ui::brand::RenderMark;
using exosnap::ui::brand::ResolveAccent;
using exosnap::ui::brand::ResolveSemantic;
using exosnap::ui::brand::ShellGlyph;
using exosnap::ui::brand::ShellGlyphRequest;
using exosnap::ui::brand::ShellIconCache;
using exosnap::ui::brand::ShellIconImageUrl;
using exosnap::ui::brand::ShellMarkRequest;

namespace {

ShellMarkRequest Mark(ShellIconState state, int px = 32) {
    ShellMarkRequest request;
    request.state = state;
    request.px = px;
    request.appearance_id = QStringLiteral("dark");
    request.accent_id = QStringLiteral("aqua");
    return request;
}

// The dominant hue of the mark's INNER half: the ring and the dot, which are
// what the state colours. Sampled rather than read from one pixel, because a
// 16 px ring is an antialiased approximation of a circle and no single pixel is
// reliably on it.
QColor InnerAverage(const QImage& image) {
    const int side = image.width();
    const int lo = side * 3 / 8;
    const int hi = side * 5 / 8;
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double weight = 0.0;
    for (int y = lo; y < hi; ++y) {
        for (int x = lo; x < hi; ++x) {
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

} // namespace

// -- determinism -------------------------------------------------------------

TEST(ShellIconRenderer, TheSameRequestProducesTheSameImage) {
    const QImage first = RenderMark(Mark(ShellIconState::Recording));
    const QImage second = RenderMark(Mark(ShellIconState::Recording));
    EXPECT_EQ(first, second);
}

TEST(ShellIconRenderer, TheRequestedSizeIsTheImageSize) {
    for (const int px : {16, 20, 24, 32, 48, 64, 256}) {
        const QImage image = RenderMark(Mark(ShellIconState::Idle, px));
        EXPECT_EQ(image.width(), px);
        EXPECT_EQ(image.height(), px);
    }
}

TEST(ShellIconRenderer, TheBackgroundIsTransparent) {
    const QImage image = RenderMark(Mark(ShellIconState::Idle, 32));
    // The corners of a 32-unit grid holding a circle of radius 14.5 are outside
    // the mark in every profile.
    EXPECT_EQ(image.pixelColor(0, 0).alpha(), 0);
    EXPECT_EQ(image.pixelColor(image.width() - 1, 0).alpha(), 0);
    EXPECT_EQ(image.pixelColor(0, image.height() - 1).alpha(), 0);
    EXPECT_EQ(image.pixelColor(image.width() - 1, image.height() - 1).alpha(), 0);
    // ... and something was actually drawn.
    EXPECT_GT(CoveredAlpha(image), 0.0);
}

// -- state semantics ---------------------------------------------------------

TEST(ShellIconRenderer, TheFourStatesAreVisiblyDifferent) {
    const QImage idle = RenderMark(Mark(ShellIconState::Idle));
    const QImage recording = RenderMark(Mark(ShellIconState::Recording));
    const QImage paused = RenderMark(Mark(ShellIconState::Paused));
    const QImage saved = RenderMark(Mark(ShellIconState::Saved));

    EXPECT_NE(idle, recording);
    EXPECT_NE(recording, paused);
    EXPECT_NE(paused, saved);
    EXPECT_NE(saved, idle);
}

TEST(ShellIconRenderer, EachStateCarriesItsOwnSemanticColourInTheInnerRing) {
    const auto& dark = exosnap::ui::theme::kExoAppearances.front();
    ASSERT_STREQ(dark.id, "dark");

    const QColor recording = InnerAverage(RenderMark(Mark(ShellIconState::Recording)));
    const QColor paused = InnerAverage(RenderMark(Mark(ShellIconState::Paused)));
    const QColor saved = InnerAverage(RenderMark(Mark(ShellIconState::Saved)));

    EXPECT_LE(HueDistance(recording, QColor(QString::fromLatin1(dark.error))), 12);
    EXPECT_LE(HueDistance(paused, QColor(QString::fromLatin1(dark.caution))), 12);
    EXPECT_LE(HueDistance(saved, QColor(QString::fromLatin1(dark.success))), 12);
}

TEST(ShellIconRenderer, IdleIsTheAccentRatherThanAState) {
    const QColor idle = InnerAverage(RenderMark(Mark(ShellIconState::Idle)));
    const QColor accent = ResolveAccent(QStringLiteral("dark"), QStringLiteral("aqua"));
    EXPECT_LE(HueDistance(idle, accent), 12);
}

TEST(ShellIconRenderer, AnAccentChangeMovesTheBrandButNotTheState) {
    ShellMarkRequest aqua = Mark(ShellIconState::Recording);
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

TEST(ShellIconRenderer, AnUnknownPaletteIdFallsBackToTheShippedDefault) {
    EXPECT_EQ(ResolveAccent(QStringLiteral("from-a-build-that-never-existed"), QStringLiteral("chartreuse")),
              ResolveAccent(QStringLiteral("dark"), QStringLiteral("aqua")));
    EXPECT_EQ(ResolveSemantic(ShellIconState::Recording, QStringLiteral("nonsense"), QStringLiteral("nonsense")),
              ResolveSemantic(ShellIconState::Recording, QStringLiteral("dark"), QStringLiteral("aqua")));
}

// -- the heartbeat -----------------------------------------------------------

TEST(ShellIconRenderer, OnlyRecordingModulatesItsOpacity) {
    for (const ShellIconState state : {ShellIconState::Idle, ShellIconState::Paused, ShellIconState::Saved}) {
        for (int frame = 0; frame < 4; ++frame)
            EXPECT_DOUBLE_EQ(PulseOpacity(state, frame), 1.0);
    }
}

TEST(ShellIconRenderer, TheBeatRisesFromATroughToAFullyWeightedPeak) {
    const double trough = PulseOpacity(ShellIconState::Recording, 0);
    const double peak = PulseOpacity(ShellIconState::Recording, exosnap::kRecordingPulsePeakFrame);
    EXPECT_GT(trough, 0.0);
    EXPECT_LT(trough, peak);
    EXPECT_DOUBLE_EQ(peak, 1.0);
}

TEST(ShellIconRenderer, TheTroughFrameDrawsLessInkThanThePeak) {
    ShellMarkRequest trough = Mark(ShellIconState::Recording, 32);
    ShellMarkRequest peak = trough;
    peak.pulse_frame = exosnap::kRecordingPulsePeakFrame;
    EXPECT_LT(CoveredAlpha(RenderMark(trough)), CoveredAlpha(RenderMark(peak)));
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
    EXPECT_GT(kSmallProfile.outer_stroke_scale, kMediumProfile.outer_stroke_scale);
    EXPECT_GT(kMediumProfile.outer_stroke_scale, kLargeProfile.outer_stroke_scale);
    EXPECT_DOUBLE_EQ(kLargeProfile.outer_stroke_scale, 1.0);
    EXPECT_DOUBLE_EQ(kLargeProfile.inner_stroke_scale, 1.0);
    EXPECT_DOUBLE_EQ(kLargeProfile.inner_radius_scale, 1.0);
    EXPECT_DOUBLE_EQ(kLargeProfile.dot_radius_scale, 1.0);
    EXPECT_DOUBLE_EQ(kLargeProfile.outer_opacity_scale, 1.0);
    EXPECT_DOUBLE_EQ(kLargeProfile.content_scale, 1.0);
}

TEST(ShellIconRendererOpticalSizing, ASmallMarkKeepsARealGapBetweenTheDotAndTheInnerRing) {
    // The reason the small profile pushes the inner ring outwards.
    //
    // At 16 px there are eight device pixels from the centre to the edge, and all
    // three elements plus two gaps have to fit in them. With the unmodified
    // radius the gap between the dot and the inner ring comes out ONE antialiased
    // column wide, and one column of partial coverage does not read as a hole:
    // the dot and the ring merge into a blob and the mark stops being an
    // aperture. The corrected geometry leaves two.
    //
    // Measured along the centre row, walking outwards from the dot.
    constexpr double kInk = 0.30;
    constexpr int kRequiredGapColumns = 2;

    const QImage image = RenderMark(Mark(ShellIconState::Idle, 16));
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

// -- glyphs ------------------------------------------------------------------

TEST(ShellIconRendererGlyphs, TheFourTransportShapesAreDistinct) {
    auto render = [](ShellGlyph glyph) {
        ShellGlyphRequest request;
        request.glyph = glyph;
        request.px = 32;
        request.appearance_id = QStringLiteral("dark");
        request.accent_id = QStringLiteral("aqua");
        return RenderGlyph(request);
    };
    const QImage record = render(ShellGlyph::Record);
    const QImage pause = render(ShellGlyph::Pause);
    const QImage resume = render(ShellGlyph::Resume);
    const QImage stop = render(ShellGlyph::Stop);

    EXPECT_NE(record, pause);
    EXPECT_NE(pause, resume);
    EXPECT_NE(resume, stop);
    EXPECT_NE(stop, record);
    for (const QImage& image : {record, pause, resume, stop}) {
        EXPECT_EQ(image.pixelColor(0, 0).alpha(), 0);
        EXPECT_GT(CoveredAlpha(image), 0.0);
    }
}

// -- image ids ---------------------------------------------------------------

TEST(ShellIconRendererIds, AMarkIdRoundTrips) {
    ShellMarkRequest request = Mark(ShellIconState::Recording, 24);
    request.pulse_frame = 3;
    request.accent_id = QStringLiteral("sky");

    ShellMarkRequest parsed;
    ASSERT_TRUE(ParseMarkImageId(MarkImageId(request), parsed));
    EXPECT_EQ(parsed.state, request.state);
    EXPECT_EQ(parsed.px, request.px);
    EXPECT_EQ(parsed.pulse_frame, request.pulse_frame);
    EXPECT_EQ(parsed.appearance_id, request.appearance_id);
    EXPECT_EQ(parsed.accent_id, request.accent_id);
}

TEST(ShellIconRendererIds, AStaticStateDropsTheHeartbeatFrameFromTheId) {
    // Otherwise the same paused icon would have four URLs, and Qt Quick's pixmap
    // cache would hold four copies of one image.
    ShellMarkRequest a = Mark(ShellIconState::Paused);
    ShellMarkRequest b = a;
    b.pulse_frame = 3;
    EXPECT_EQ(MarkImageId(a), MarkImageId(b));
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
    EXPECT_FALSE(ParseMarkImageId(QStringLiteral("mark/recording/32/0/dark"), mark));
    EXPECT_FALSE(ParseMarkImageId(QStringLiteral("mark/spinning/32/0/dark/aqua"), mark));
    EXPECT_FALSE(ParseMarkImageId(QStringLiteral("mark/recording/big/0/dark/aqua"), mark));
    EXPECT_FALSE(ParseMarkImageId(QStringLiteral("glyph/pause/16/dark/aqua"), mark));
    EXPECT_FALSE(ParseGlyphImageId(QStringLiteral("glyph/rewind/16/dark/aqua"), glyph));
}

TEST(ShellIconRendererIds, TheUrlAddressesTheProvider) {
    EXPECT_EQ(ShellIconImageUrl(QStringLiteral("mark/idle/16/0/dark/aqua")),
              QStringLiteral("image://exosnap-shell/mark/idle/16/0/dark/aqua"));
}

// -- the cache ---------------------------------------------------------------

TEST(ShellIconRendererCache, OneEntryPerDistinctKey) {
    ShellIconCache cache;
    const ShellMarkRequest recording = Mark(ShellIconState::Recording);

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

TEST(ShellIconRendererCache, ACachedImageIsTheRenderedOne) {
    ShellIconCache cache;
    const ShellMarkRequest request = Mark(ShellIconState::Saved, 20);
    EXPECT_EQ(cache.mark(request), RenderMark(request));
    EXPECT_EQ(cache.mark(request), RenderMark(request));
}

TEST(ShellIconRendererCache, NoThemeVariantCanGoStale) {
    // The palette ids are part of the key, so a changed accent cannot be served
    // the previous accent's raster -- which is the one cache bug a tray icon
    // would show for the rest of the session.
    ShellIconCache cache;
    ShellMarkRequest aqua = Mark(ShellIconState::Idle);
    ShellMarkRequest magenta = aqua;
    magenta.accent_id = QStringLiteral("magenta");

    const QImage first = cache.mark(aqua);
    const QImage second = cache.mark(magenta);
    EXPECT_NE(first, second);
    EXPECT_EQ(cache.mark(aqua), first);
}

TEST(ShellIconRendererCache, ClearEmptiesIt) {
    ShellIconCache cache;
    (void)cache.mark(Mark(ShellIconState::Idle));
    ASSERT_GT(cache.sizeForTest(), 0);
    cache.clear();
    EXPECT_EQ(cache.sizeForTest(), 0);
}
