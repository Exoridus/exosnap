// Contrast gate for the shipped appearance x accent matrix.
//
// Separate from the semantic hue-distance test in test_settings_adapter.cpp:
// that one asks "can an accent be mistaken for a state?", this one asks "can the
// content be read at all?". Both run over every shipped combination, and both
// read the resolved tokens rather than the table, so a derivation rule that
// quietly changes a colour is caught here too.
//
// Thresholds by role, not one blanket ratio
// -----------------------------------------
// WCAG 2.1 sets different bars for different things, and applying the strictest
// one everywhere would force palette changes where the criterion does not apply
// (and would still say nothing about whether the result is usable):
//
//   * 1.4.3 Contrast (Minimum), normal text .......................... 4.5:1
//     Body text, labels, secondary metadata a user actually reads, and the ink
//     on a filled accent control. ExoSnap's largest button label is 16 px
//     DemiBold, which is NOT "large text" by 1.4.3 (that starts at 18.66 px
//     bold / 24 px regular), so the whole product sits on the 4.5 bar.
//
//   * 1.4.11 Non-text Contrast, UI components and graphical objects ... 3:1
//     The *indicator* that identifies a control's state — the accent underline
//     under the selected nav tab, the accent ring on an active dock toggle, the
//     keyboard focus ring. Not the tinted fill behind them: the fill reinforces
//     a state the border and the icon already carry above 4.5:1, so it is
//     decorative under the criterion's own "visual information REQUIRED to
//     identify" wording. Testing the fill would demand a palette that shouts.
//
//   * Inactive components are explicitly EXEMPT from both 1.4.3 and 1.4.11.
//     A disabled control therefore has no accessibility floor at all. It does
//     have a product one: ExoSnap keeps a source that cannot be used visible
//     rather than hiding it (product-spec §8), which is worthless if it cannot
//     be seen. That promise is pinned at the 3:1 graphical bar rather than left
//     to judgement.
//
// A resting hairline (`line`) is deliberately NOT tested against 3:1. It is
// separation between surfaces, not the information that identifies a control —
// which the fill step, the icon and the focus ring carry — and every calm
// desktop palette, this one included, sits near 1.2:1 there. Holding it to 3:1
// would mean redrawing every border in the product to satisfy a criterion it is
// not the subject of.

#include "QuickThemeTokens.h"

#include <gtest/gtest.h>

#include <QColor>
#include <QString>
#include <QVariantMap>

#include <cmath>
#include <string>
#include <vector>

using exosnap::quick::QuickThemeTokens;

namespace {

// WCAG 2.1 relative luminance.
double relativeLuminance(const QColor& color) {
    const auto channel = [](double value) {
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
}

// WCAG 2.1 contrast ratio. Both colours must already be opaque — see composite().
double contrastRatio(const QColor& a, const QColor& b) {
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// Source-over of a possibly translucent colour onto an opaque backdrop. Several
// tokens carry alpha (the line tokens) and QML composes several more at runtime
// (the accent tint on an active control), and a contrast ratio computed from an
// unresolved alpha is meaningless.
QColor composite(const QColor& foreground, double alpha, const QColor& backdrop) {
    const double a = foreground.alphaF() * alpha;
    return QColor::fromRgbF(foreground.redF() * a + backdrop.redF() * (1.0 - a),
                            foreground.greenF() * a + backdrop.greenF() * (1.0 - a),
                            foreground.blueF() * a + backdrop.blueF() * (1.0 - a));
}

constexpr double kText = 4.5;      // WCAG 1.4.3, normal text
constexpr double kGraphical = 3.0; // WCAG 1.4.11, UI components and graphical objects

struct Combination {
    QString appearance;
    QString accent;

    [[nodiscard]] std::string name() const {
        return (appearance + QLatin1Char('+') + accent).toStdString();
    }
};

std::vector<Combination> shippedCombinations() {
    std::vector<Combination> combinations;
    for (const QVariant& appearance_entry : QuickThemeTokens::appearanceOptions()) {
        const QString appearance = appearance_entry.toMap().value(QStringLiteral("value")).toString();
        for (const QVariant& accent_entry : QuickThemeTokens::accentOptions(appearance)) {
            combinations.push_back({appearance, accent_entry.toMap().value(QStringLiteral("value")).toString()});
        }
    }
    return combinations;
}

// Fails with both the ratio and the two hex values, because "3.94 < 4.5" alone
// does not say which token to move.
::testing::AssertionResult meets(double required, const QColor& foreground, const QColor& background,
                                 const char* role) {
    const double ratio = contrastRatio(foreground, background);
    if (ratio >= required)
        return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << role << ": " << foreground.name().toStdString() << " on "
                                         << background.name().toStdString() << " is " << ratio << ":1, needs "
                                         << required << ":1";
}

} // namespace

// ── Text: WCAG 1.4.3, 4.5:1 ─────────────────────────────────────────────────

TEST(ThemeContrastTest, PrimaryTextIsReadableOnEverySurfaceRung) {
    for (const Combination& combination : shippedCombinations()) {
        QuickThemeTokens tokens;
        tokens.setAppearance(combination.appearance, combination.accent);
        SCOPED_TRACE(combination.name());

        EXPECT_TRUE(meets(kText, tokens.text(), tokens.background(), "text on background"));
        EXPECT_TRUE(meets(kText, tokens.text(), tokens.surface(), "text on surface"));
        EXPECT_TRUE(meets(kText, tokens.text(), tokens.surfaceRaised(), "text on surfaceRaised"));
        EXPECT_TRUE(meets(kText, tokens.text(), tokens.surfaceHover(), "text on surfaceHover"));
    }
}

TEST(ThemeContrastTest, SecondaryAndMutedTextAreReadableOnTheirSurfaces) {
    // Both are text rungs, not decoration: `textSecondary` is an unselected nav
    // tab and a button label, `textMuted` is a setting's hint line and the
    // preview toolbar's format summary. Neither gets a discount for being quiet.
    for (const Combination& combination : shippedCombinations()) {
        QuickThemeTokens tokens;
        tokens.setAppearance(combination.appearance, combination.accent);
        SCOPED_TRACE(combination.name());

        EXPECT_TRUE(meets(kText, tokens.textSecondary(), tokens.background(), "textSecondary on background"));
        EXPECT_TRUE(meets(kText, tokens.textSecondary(), tokens.surface(), "textSecondary on surface"));
        EXPECT_TRUE(meets(kText, tokens.textSecondary(), tokens.surfaceRaised(), "textSecondary on surfaceRaised"));

        EXPECT_TRUE(meets(kText, tokens.textMuted(), tokens.background(), "textMuted on background"));
        EXPECT_TRUE(meets(kText, tokens.textMuted(), tokens.surface(), "textMuted on surface"));
        EXPECT_TRUE(meets(kText, tokens.textMuted(), tokens.surfaceRaised(), "textMuted on surfaceRaised"));
    }
}

TEST(ThemeContrastTest, AccentInkIsReadableOnFilledAccentControls) {
    // The Record pill, the primary ExoButton, the Resume action. `accentInk` is
    // curated per accent and per appearance precisely so this holds; a derived
    // ink would fail here for at least one of the eight combinations.
    for (const Combination& combination : shippedCombinations()) {
        QuickThemeTokens tokens;
        tokens.setAppearance(combination.appearance, combination.accent);
        SCOPED_TRACE(combination.name());

        EXPECT_TRUE(meets(kText, tokens.accentInk(), tokens.accent(), "accentInk on accent"));
        // Hover and press tint the same fill (ExoTheme.hoverTint / pressTint):
        // dark lightens by 14 %, both directions darken on press. The ink has to
        // survive the extreme it actually reaches.
        const QColor hovered = tokens.dark() ? tokens.accent().lighter(114) : tokens.accent().darker(105);
        const QColor pressed = tokens.accent().darker(tokens.dark() ? 110 : 112);
        EXPECT_TRUE(meets(kText, tokens.accentInk(), hovered, "accentInk on hovered accent"));
        EXPECT_TRUE(meets(kText, tokens.accentInk(), pressed, "accentInk on pressed accent"));
    }
}

TEST(ThemeContrastTest, ErrorInkIsReadableOnTheFilledStopAction) {
    // The Stop pill is `error`-filled with `errorInk` on it, and it is the one
    // control a user reaches for under time pressure.
    for (const Combination& combination : shippedCombinations()) {
        QuickThemeTokens tokens;
        tokens.setAppearance(combination.appearance, combination.accent);
        SCOPED_TRACE(combination.name());

        EXPECT_TRUE(meets(kText, tokens.errorInk(), tokens.error(), "errorInk on error"));
    }
}

// ── Indicators: WCAG 1.4.11, 3:1 ────────────────────────────────────────────

TEST(ThemeContrastTest, SelectionAndFocusIndicatorsMeetTheGraphicalThreshold) {
    for (const Combination& combination : shippedCombinations()) {
        QuickThemeTokens tokens;
        tokens.setAppearance(combination.appearance, combination.accent);
        SCOPED_TRACE(combination.name());

        // The selected nav tab's accent underline, drawn on the title band —
        // which is the page background, not a card.
        EXPECT_TRUE(meets(kGraphical, tokens.accent(), tokens.background(), "nav underline on background"));
        // The active dock toggle's accent ring, drawn on the dock (`surface`),
        // and the same ring on a card (`surfaceRaised`) for a selected control.
        EXPECT_TRUE(meets(kGraphical, tokens.accent(), tokens.surface(), "active ring on dock surface"));
        EXPECT_TRUE(meets(kGraphical, tokens.accent(), tokens.surfaceRaised(), "selected ring on surfaceRaised"));
        // The keyboard focus ring is `text`, everywhere.
        EXPECT_TRUE(meets(kGraphical, tokens.text(), tokens.surface(), "focus ring on surface"));
        EXPECT_TRUE(meets(kGraphical, tokens.text(), tokens.surfaceRaised(), "focus ring on surfaceRaised"));
        // The accent swatch's selection ring in Settings, which is `text` on the
        // card the swatches sit on.
        EXPECT_TRUE(meets(kGraphical, tokens.text(), tokens.surfaceHover(), "selection ring on surfaceHover"));
    }
}

TEST(ThemeContrastTest, AnActiveDockControlIsIdentifiableByItsIcon) {
    // The icon rides on the accent-tinted fill (ExoTheme.dockFill: the accent at
    // 22 % over the raised surface). That icon and the ring above are what carry
    // the state; the tint itself is reinforcement and is deliberately not held
    // to 3:1, or the dock would have to shout to pass.
    for (const Combination& combination : shippedCombinations()) {
        QuickThemeTokens tokens;
        tokens.setAppearance(combination.appearance, combination.accent);
        SCOPED_TRACE(combination.name());

        const QColor active_fill = composite(tokens.accent(), 0.22, tokens.surfaceRaised());
        EXPECT_TRUE(meets(kGraphical, tokens.accent(), active_fill, "active icon on tinted fill"));
    }
}

// ── Unavailable: exempt from WCAG, held to a product floor ──────────────────

TEST(ThemeContrastTest, AnUnavailableControlStaysVisible) {
    // ExoTheme.dockFill/dockInk resolve an unavailable control to `textDim` on
    // `surface` — flat with the dock, dimmest ink. WCAG exempts inactive
    // components entirely; this is the product's own promise that a source which
    // cannot be used stays VISIBLE and disabled rather than disappearing
    // (product-spec §8), pinned at the graphical bar so it is not left to taste.
    for (const Combination& combination : shippedCombinations()) {
        QuickThemeTokens tokens;
        tokens.setAppearance(combination.appearance, combination.accent);
        SCOPED_TRACE(combination.name());

        EXPECT_TRUE(meets(kGraphical, tokens.textDim(), tokens.surface(), "disabled dock ink on dock fill"));
        // ExoButton's disabled state is the same ink on the raised surface.
        EXPECT_TRUE(meets(kGraphical, tokens.textDim(), tokens.surfaceRaised(), "disabled button ink on button fill"));
        EXPECT_TRUE(meets(kGraphical, tokens.textDim(), tokens.background(), "disabled ink on background"));
    }
}

// The floor above has NO headroom: `dim` was nudged twice, once per appearance,
// specifically to land just over 3:1 on the surfaces it is drawn on. So the one
// way to break it without touching a colour is to fade the control that carries
// it — and a shared `opacity: disabledOpacity` on ExoButton did exactly that,
// while the assertions above kept passing because they read the token rather
// than what reaches the screen.
//
// This test asserts the trap rather than the result: if the disabled rung is
// ever applied to a control's INK again, the arithmetic here is what says how
// far under the floor that lands. `disabledOpacity` belongs on chrome — a fill
// and a border that recede — where it costs the ink nothing.
TEST(ThemeContrastTest, TheDisabledRungMayNotBeAppliedToInk) {
    // ExoTheme.disabledOpacity.
    constexpr double kDisabledOpacity = 0.45;

    for (const Combination& combination : shippedCombinations()) {
        QuickThemeTokens tokens;
        tokens.setAppearance(combination.appearance, combination.accent);
        SCOPED_TRACE(combination.name());

        const QColor faded = composite(tokens.textDim(), kDisabledOpacity, tokens.surfaceRaised());
        const double ratio = contrastRatio(faded, tokens.surfaceRaised());
        EXPECT_LT(ratio, kGraphical) << "textDim at " << kDisabledOpacity << " now measures " << ratio
                                     << ":1 on surfaceRaised. If the palette moved far enough that fading "
                                        "the ink is survivable, this test is the thing to revisit — not "
                                        "ExoButton, which deliberately fades only its background.";
    }
}

TEST(ThemeContrastTest, AnUnavailableButActiveControlStillReadsAsOn) {
    // The transport locks its source toggles for the whole recording, so this is
    // the state a user spends a recording looking at. dockInk/dockBorder keep a
    // muted accent there; both must still clear the graphical bar against the
    // fill they are drawn on, or "which sources am I recording?" becomes
    // unanswerable exactly when it matters.
    for (const Combination& combination : shippedCombinations()) {
        QuickThemeTokens tokens;
        tokens.setAppearance(combination.appearance, combination.accent);
        SCOPED_TRACE(combination.name());

        const QColor locked_fill = composite(tokens.accent(), 0.12, tokens.surface());

        EXPECT_TRUE(meets(kGraphical, tokens.accent(), locked_fill, "locked-on icon on its fill"));
        EXPECT_TRUE(meets(kGraphical, tokens.accent(), tokens.surface(), "locked-on ring on dock surface"));
    }
}

// ── The states themselves ───────────────────────────────────────────────────

TEST(ThemeContrastTest, StateColoursAreVisibleAsIndicatorsOnEverySurface) {
    // Recording coral, caution amber and ready green are drawn as rings, dots
    // and pill grounds rather than as body text, so the graphical bar is the one
    // that applies. They live on the appearance, never on the accent, so this is
    // constant across the four accents — which is exactly what makes iterating
    // over all eight combinations a regression check rather than duplication.
    for (const Combination& combination : shippedCombinations()) {
        QuickThemeTokens tokens;
        tokens.setAppearance(combination.appearance, combination.accent);
        SCOPED_TRACE(combination.name());

        for (const auto& [state, color] : {std::pair{"error", tokens.error()}, std::pair{"warning", tokens.warning()},
                                           std::pair{"success", tokens.success()}}) {
            EXPECT_TRUE(meets(kGraphical, color, tokens.background(), state));
            EXPECT_TRUE(meets(kGraphical, color, tokens.surface(), state));
            EXPECT_TRUE(meets(kGraphical, color, tokens.surfaceRaised(), state));
        }
    }
}

// The status pill that sits ON the live preview (ExoStatusPill.onSurface) is the
// one surface in the product whose ground is near-black in BOTH appearances: it
// has to be, because what is behind it is arbitrary captured content. That makes
// every appearance colour on it suspect, and it is why the label there takes a
// literal light ink while the dot and the ring carry the tone.
TEST(ThemeContrastTest, TheStatusPillOverThePreviewStaysReadableInBothAppearances) {
    // ExoStatusPill: Qt.rgba(0, 0, 0, 0.72) over the Preview Surface's own
    // #08080A stage.
    const QColor stage(QStringLiteral("#08080A"));
    const QColor ground = composite(QColor(0, 0, 0), 0.72, stage);
    const QColor on_surface_ink(QStringLiteral("#F1F1EF")); // ExoStatusPill.onSurfaceInk

    for (const Combination& combination : shippedCombinations()) {
        QuickThemeTokens tokens;
        tokens.setAppearance(combination.appearance, combination.accent);
        SCOPED_TRACE(combination.name());

        EXPECT_TRUE(meets(kText, on_surface_ink, ground, "pill label on the preview ground"));

        // Every tone the pill can resolve to over this ground, as an indicator
        // (dot + ring). `busy` resolves to the light ink here rather than to
        // textMuted precisely because Light's textMuted measures 2.998:1 on it.
        for (const auto& [state, color] :
             {std::pair{"recording/error", tokens.error()}, std::pair{"warning", tokens.warning()},
              std::pair{"success/neutral", tokens.success()}, std::pair{"paused", tokens.accent()},
              std::pair{"busy", on_surface_ink}}) {
            EXPECT_TRUE(meets(kGraphical, color, ground, state));
        }

        // …and in the title band the same tone sits on an appearance surface,
        // where textMuted is the right neutral and clears the bar.
        EXPECT_TRUE(meets(kGraphical, tokens.textMuted(), tokens.background(), "busy dot in the title band"));
    }
}

// Stop gives up its fill while a recording is paused (Resume is the state's one
// recommended action there) and states itself in the error colour instead, as
// text on the dock's raised control fill.
TEST(ThemeContrastTest, AnOutlinedDestructiveActionStatesItselfInReadableInk) {
    for (const Combination& combination : shippedCombinations()) {
        QuickThemeTokens tokens;
        tokens.setAppearance(combination.appearance, combination.accent);
        SCOPED_TRACE(combination.name());

        EXPECT_TRUE(meets(kText, tokens.error(), tokens.surfaceRaised(), "outlined Stop label"));
        EXPECT_TRUE(meets(kGraphical, tokens.error(), tokens.surfaceRaised(), "outlined Stop border"));
    }
}

// ── QCR-501: fixed-dark surfaces ────────────────────────────────────────────
//
// Six surfaces in the product are near-black in BOTH appearances because what
// is behind them is not the application: the five capture-excluded overlays
// (recording pill, diagnostics pill, countdown, quick-control pill, and the
// desktop toast's tone fills) and the readouts drawn over the live preview.
// They resolve their colours against the Dark appearance, so `overlayInk` and
// friends must clear the bars on every one of those grounds in BOTH
// appearances — which is the same assertion twice by construction, and that is
// the point: the test fails the moment one of them goes back to an appearance
// token.
namespace {

// The grounds, each already composited over the darkest thing behind it. That
// is the design's own assumption (see OverlayRecording.qml: a near-black pill
// at ~78 % opacity, in a screen corner) and it is what the pre-existing status
// pill assertion below already uses. A fully white desktop behind a 78 % pill
// lifts it to ~#4A4A4C, where `overlayInk` still measures 7.8:1 — the ink
// survives that; the quieter rungs do not, and no ink can, because the ground
// is then a mid grey. Changing that would mean changing the overlays' opacity,
// which is a design decision this gate does not make.
struct FixedDarkGround {
    const char* name;
    QColor color;
};

std::vector<FixedDarkGround> fixedDarkGrounds() {
    return {
        // OverlayRecording / OverlayDiagnostics: "#C6161618" over black.
        {"overlay pill", composite(QColor(QStringLiteral("#161618")), 198.0 / 255.0, QColor(0, 0, 0))},
        // RecordPage's liveMetrics: rgba(0,0,0,0.72) over the "#08080A" stage.
        {"live metrics", composite(QColor(0, 0, 0), 0.72, QColor(QStringLiteral("#08080A")))},
        // RecordPage's PreviewMetricsOverlay: "#E6151517" over the same stage.
        {"preview metrics",
         composite(QColor(QStringLiteral("#151517")), 230.0 / 255.0, QColor(QStringLiteral("#08080A")))},
        // OverlayCountdown: "#BD0C0C0E" over black.
        {"countdown circle", composite(QColor(QStringLiteral("#0C0C0E")), 189.0 / 255.0, QColor(0, 0, 0))},
        // OverlayQuickControlPill: "#CC0C0C0E" over black.
        {"quick-control pill", composite(QColor(QStringLiteral("#0C0C0E")), 204.0 / 255.0, QColor(0, 0, 0))},
    };
}

} // namespace

TEST(ThemeContrastTest, FixedDarkSurfaceInkIsReadableInBothAppearances) {
    for (const Combination& combination : shippedCombinations()) {
        QuickThemeTokens tokens;
        tokens.setAppearance(combination.appearance, combination.accent);
        SCOPED_TRACE(combination.name());

        for (const FixedDarkGround& ground : fixedDarkGrounds()) {
            SCOPED_TRACE(ground.name);
            // The three ink rungs are all TEXT on these surfaces: the elapsed
            // clock, the output size, a diagnostics token's label and value.
            EXPECT_TRUE(meets(kText, QuickThemeTokens::overlayInk(), ground.color, "overlayInk"));
            EXPECT_TRUE(meets(kText, QuickThemeTokens::overlayInkSecondary(), ground.color, "overlayInkSecondary"));
            EXPECT_TRUE(meets(kText, QuickThemeTokens::overlayInkMuted(), ground.color, "overlayInkMuted"));
            // `overlaySuccess` is the diagnostics pill's "drop 0" VALUE, so it
            // is text too. The other two carry the recording pill's state glyph
            // and the Stop control's fill/ring, which are graphical.
            EXPECT_TRUE(meets(kText, QuickThemeTokens::overlaySuccess(), ground.color, "overlaySuccess"));
            EXPECT_TRUE(meets(kGraphical, QuickThemeTokens::overlayWarning(), ground.color, "overlayWarning"));
            EXPECT_TRUE(meets(kGraphical, QuickThemeTokens::overlayError(), ground.color, "overlayError"));
            // The countdown digit is 52 px, well past 1.4.3's large-text
            // threshold, but the ring beside it is a graphical object and the
            // preview metrics panel's border is another — 3:1 covers both, and
            // every accent clears it by 3x in its dark resolution.
            EXPECT_TRUE(meets(kGraphical, tokens.overlayAccent(), ground.color, "overlayAccent"));
        }
    }
}

// The failure this whole item is about, asserted directly rather than only
// implied by the passing case above: the APPEARANCE ink on a fixed-dark ground
// is unreadable in Light. If a future palette made Light's ink light enough to
// survive there, the fixed-dark tokens would be arguing for nothing and this
// test is what says so.
TEST(ThemeContrastTest, TheAppearanceInkIsWhyTheFixedDarkRungsExist) {
    QuickThemeTokens light;
    light.setAppearance(QStringLiteral("light"), QStringLiteral("aqua"));
    ASSERT_FALSE(light.dark());

    for (const FixedDarkGround& ground : fixedDarkGrounds()) {
        SCOPED_TRACE(ground.name);
        EXPECT_LT(contrastRatio(light.text(), ground.color), kGraphical)
            << "Light's `text` now measures " << contrastRatio(light.text(), ground.color)
            << ":1 on a fixed-dark surface. That is the assumption the overlayInk rungs were "
               "introduced under; revisit them rather than deleting this test.";
        EXPECT_LT(contrastRatio(light.textSecondary(), ground.color), kText);
    }
}

// ── QCR-502: the semantic text rungs ────────────────────────────────────────

TEST(ThemeContrastTest, SemanticTextRungsAreReadableOnEverySurfaceTheyLandOn) {
    // A state used as a WORD (a badge label, a severity glyph inside its own
    // tinted card, a log row's severity column, a failed export's verdict) is
    // text, and the indicator values do not clear 4.5:1 in Light. The tinted
    // grounds are the binding case: they are the darkest surfaces a dark ink
    // is drawn on.
    for (const Combination& combination : shippedCombinations()) {
        QuickThemeTokens tokens;
        tokens.setAppearance(combination.appearance, combination.accent);
        SCOPED_TRACE(combination.name());

        for (const auto& [state, color] :
             {std::pair{"successText", tokens.successText()}, std::pair{"warningText", tokens.warningText()},
              std::pair{"errorText", tokens.errorText()}}) {
            EXPECT_TRUE(meets(kText, color, tokens.background(), state));
            EXPECT_TRUE(meets(kText, color, tokens.surface(), state));
            EXPECT_TRUE(meets(kText, color, tokens.surfaceRaised(), state));
            EXPECT_TRUE(meets(kText, color, tokens.surfaceHover(), state));
            EXPECT_TRUE(meets(kText, color, tokens.warningSurface(), state));
            EXPECT_TRUE(meets(kText, color, tokens.errorSurface(), state));
        }
    }
}

// The reason the split exists, pinned the same way the fixed-dark one is: the
// INDICATOR values are below the text bar in Light. Dark needs no split at all,
// and the table says so by repeating its three values — this test is what would
// catch a Light palette drifting far enough to make the second rung redundant.
TEST(ThemeContrastTest, TheIndicatorRungIsWhyTheSemanticTextRungsExist) {
    QuickThemeTokens light;
    light.setAppearance(QStringLiteral("light"), QStringLiteral("aqua"));

    EXPECT_LT(contrastRatio(light.success(), light.surface()), kText);
    EXPECT_LT(contrastRatio(light.warning(), light.surface()), kText);
    EXPECT_LT(contrastRatio(light.warning(), light.warningSurface()), kGraphical);

    QuickThemeTokens dark;
    dark.setAppearance(QStringLiteral("dark"), QStringLiteral("aqua"));
    EXPECT_EQ(dark.successText(), dark.success());
    EXPECT_EQ(dark.warningText(), dark.warning());
    EXPECT_EQ(dark.errorText(), dark.error());
}

TEST(ThemeContrastTest, EveryToneFilledActionCarriesReadableInk) {
    // The desktop toast's primary action is filled with the toast's own tone,
    // and it used to draw one literal near-black label on all four fills. The
    // theme curates an ink per fill instead; `successInk`/`warningInk` complete
    // the set `accentInk`/`errorInk` already covered.
    for (const Combination& combination : shippedCombinations()) {
        QuickThemeTokens tokens;
        tokens.setAppearance(combination.appearance, combination.accent);
        SCOPED_TRACE(combination.name());

        EXPECT_TRUE(meets(kText, tokens.successInk(), tokens.success(), "successInk on success"));
        EXPECT_TRUE(meets(kText, tokens.warningInk(), tokens.warning(), "warningInk on warning"));
    }
}

TEST(ThemeContrastTest, TintedNoticeGroundsCarryReadableText) {
    // warningSurface / errorSurface are derived (blend of the background and the
    // state colour), so unlike every pair above they are not reviewable by
    // reading the table. A notice is a paragraph, so the text bar applies.
    for (const Combination& combination : shippedCombinations()) {
        QuickThemeTokens tokens;
        tokens.setAppearance(combination.appearance, combination.accent);
        SCOPED_TRACE(combination.name());

        EXPECT_TRUE(meets(kText, tokens.text(), tokens.warningSurface(), "text on warningSurface"));
        EXPECT_TRUE(meets(kText, tokens.text(), tokens.errorSurface(), "text on errorSurface"));
    }
}
