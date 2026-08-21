pragma Singleton

import QtQuick

QtObject {
    id: root

    // Colours come from the shared theme table via QuickThemeTokens, so all four
    // shipped themes resolve through one definition. Metrics and fonts below are
    // theme-independent and stay here.
    readonly property color background: QuickThemeTokens.background
    readonly property color surface: QuickThemeTokens.surface
    readonly property color surfaceRaised: QuickThemeTokens.surfaceRaised
    readonly property color surfaceHover: QuickThemeTokens.surfaceHover
    readonly property color line: QuickThemeTokens.line
    readonly property color lineStrong: QuickThemeTokens.lineStrong
    readonly property color text: QuickThemeTokens.text
    readonly property color textSecondary: QuickThemeTokens.textSecondary
    readonly property color textMuted: QuickThemeTokens.textMuted
    readonly property color textDim: QuickThemeTokens.textDim
    readonly property color accent: QuickThemeTokens.accent
    readonly property color accentInk: QuickThemeTokens.accentInk
    readonly property color warning: QuickThemeTokens.warning
    readonly property color warningSurface: QuickThemeTokens.warningSurface
    readonly property color error: QuickThemeTokens.error
    readonly property color errorInk: QuickThemeTokens.errorInk
    readonly property color errorSurface: QuickThemeTokens.errorSurface
    readonly property color success: QuickThemeTokens.success
    readonly property color successInk: QuickThemeTokens.successInk
    readonly property color warningInk: QuickThemeTokens.warningInk

    // The three states as READABLE TEXT, as opposed to as an indicator.
    //
    // `warning`/`success`/`error` are tuned to carry a state as a ring, a dot
    // or a tinted ground, where WCAG 1.4.11 asks 3:1. The same values as small
    // text sit at 3.3–3.9:1 in Light, under the 4.5:1 of 1.4.3 — so a badge
    // label, a severity glyph inside its own tinted card, or any sentence that
    // states its own severity takes the rung below instead. In Dark the two
    // rungs are the same colour; the split exists because Light cannot have
    // one value that does both jobs.
    readonly property color successText: QuickThemeTokens.successText
    readonly property color warningText: QuickThemeTokens.warningText
    readonly property color errorText: QuickThemeTokens.errorText

    // ── Fixed-dark surfaces ──────────────────────────────────────────────────
    //
    // For a surface whose ground is near-black in BOTH appearances: the five
    // capture-excluded overlays over the desktop, and the readouts over the
    // live preview. See QuickThemeTokens.h — these resolve against the Dark
    // appearance, so the application appearance never turns their ink dark.
    readonly property color overlayInk: QuickThemeTokens.overlayInk
    readonly property color overlayInkSecondary: QuickThemeTokens.overlayInkSecondary
    readonly property color overlayInkMuted: QuickThemeTokens.overlayInkMuted
    readonly property color overlayAccent: QuickThemeTokens.overlayAccent
    readonly property color overlaySurface: QuickThemeTokens.overlaySurface
    readonly property color overlaySurfaceRaised: QuickThemeTokens.overlaySurfaceRaised
    readonly property color overlayLine: QuickThemeTokens.overlayLine
    readonly property color overlayLineStrong: QuickThemeTokens.overlayLineStrong
    readonly property color overlayInkDim: QuickThemeTokens.overlayInkDim
    readonly property color overlaySuccess: QuickThemeTokens.overlaySuccess
    readonly property color overlayWarning: QuickThemeTokens.overlayWarning
    readonly property color overlayError: QuickThemeTokens.overlayError

    // The ground a modal/interruption surface lays over the application. Its
    // meaning is "de-emphasise what is behind this", which is why it is a token
    // and not `Qt.alpha(background, x)` at each call site: a translucent copy of
    // the page background de-emphasises nothing in Light.
    readonly property color overlayScrim: QuickThemeTokens.overlayScrim
    readonly property bool dark: QuickThemeTokens.dark

    // ── Spacing scale ────────────────────────────────────────────────────────
    readonly property int spacingXs: 4
    readonly property int spacingSm: 8
    readonly property int spacingMd: 12
    readonly property int spacingLg: 16
    readonly property int spacingXl: 24
    readonly property int spacing2Xl: 32

    // Named density values. These are the ones that decide how much configuration
    // fits on screen, so they are separate from the raw scale: tuning the product's
    // density must not silently retune every unrelated gap that happens to share a
    // number today.
    //
    // These were once tuned DOWN, to fit more configuration on one screen. Judged
    // against the Widgets reference side by side that read as a window rendered at
    // reduced zoom rather than as a compact desktop application, so the scale is
    // back at desktop density. "How much fits without scrolling" is explicitly not
    // the metric being optimised here.
    readonly property int rowSpacing: 12       // between setting rows inside a card
    readonly property int sectionGap: 18       // between cards on a page
    readonly property int cardPadding: 18      // a card's own inset
    readonly property int cardPaddingCompact: 14
    readonly property int pagePadding: 24      // page edge inset

    // ── Radii ────────────────────────────────────────────────────────────────
    //
    // Four rungs and nothing between them. `radiusXs` is for something that is
    // barely taller than its own corner — a badge, the selected chip inside a
    // segmented control — where radiusSm rounds so far it reads as a pill it is
    // not. It is not a new value: it names the 6 both of those already drew.
    // Anything smaller still in the tree (a 3 px dot, a 1 px caret) is an
    // indicator, not a container, and deliberately stays local.
    readonly property int radiusXs: 6
    readonly property int radiusSm: 8
    readonly property int radiusMd: 10
    readonly property int radiusLg: 14
    readonly property int radiusPill: 999

    // ── Control sizing ───────────────────────────────────────────────────────
    //
    // 36 is the Widgets shell's ui::theme::ExoSnapMetrics::kControlHeight and the
    // floor for a comfortable desktop hit target; 44 is its kPrimaryCtaHeight, so
    // the one recommended action on a surface is visibly larger than the row of
    // secondary ones next to it.
    readonly property int controlHeight: 36
    readonly property int controlHeightCompact: 30
    readonly property int controlHeightLarge: 44

    // ── Typography ───────────────────────────────────────────────────────────
    // Semantic rungs, not a numeric scale: the caller names the role and the theme
    // decides the size, which is what keeps one page from inventing a fourth
    // heading size.
    //
    // Six rungs that must stay TELLABLE APART at 100% scaling. The previous set
    // spanned 11..19 and collapsed body, section title and secondary into what
    // read as one small size; the spread below restores the steps between them.
    // The mono kicker: an uppercase label over a value, the text inside a badge,
    // a section eyebrow, a track name on the timeline. Deliberately below
    // fontCaption — it labels, it is never read as prose. It is a named rung
    // because the same role was being drawn at 9 px in four files and 10 px in a
    // dozen others, which is a size difference nobody chose.
    readonly property int fontEyebrow: 10
    readonly property int fontCaption: 12      // hints, units, secondary metadata
    readonly property int fontSecondary: 13    // subtitles, muted supporting text
    readonly property int fontBody: 14         // labels, controls, list rows
    readonly property int fontSectionTitle: 16 // card / section headings
    readonly property int fontBrand: 16        // the shell's wordmark
    readonly property int fontPageTitle: 22    // one per page, at most

    // The number the surface EXISTS to show: a diagnostics tile's measurement, the
    // transport's elapsed time, a readiness count. Deliberately far above
    // fontSectionTitle — a value that reads at the same weight as its own label is
    // the specific failure this rung was added to fix.
    readonly property int fontValue: 22
    readonly property int fontValueLarge: 28

    // ── State treatment ──────────────────────────────────────────────────────
    readonly property real disabledOpacity: 0.45
    readonly property int focusRingWidth: 1
    readonly property int animFast: 90         // state flips: switches, hovers
    readonly property int animMedium: 140      // disclosure, toast enter/leave

    // Hover/press are a lift and a press of the SAME surface, so a light theme
    // must not lighten what is already near-white. One rule, both directions.
    function hoverTint(base: color): color {
        return root.dark ? Qt.lighter(base, 1.14) : Qt.darker(base, 1.05);
    }

    function pressTint(base: color): color {
        return root.dark ? Qt.darker(base, 1.10) : Qt.darker(base, 1.12);
    }

    // An accent-tinted version of a surface, for a control that is ON. Blended
    // into the surface rather than laid over it at alpha, so the same call
    // produces one readable step in both appearances instead of washing a light
    // surface out.
    function accentTint(base: color, strength: real): color {
        return Qt.tint(base, Qt.rgba(root.accent.r, root.accent.g, root.accent.b, strength));
    }

    // ── Transport dock controls ──────────────────────────────────────────────
    //
    // The dock is the recessed base and its round controls sit ON it. It used to
    // be the other way round — a raised bar with controls a step DARKER than it —
    // which read as eight holes punched into the transport rather than as eight
    // buttons. These three functions are the whole relationship, in one place,
    // because the source toggles and the action buttons are peers on the same bar
    // and had drifted into three different treatments between them.
    //
    // Available and unavailable are deliberately far apart, on two cues at once:
    // an unavailable control drops to the dock's own fill (so it is not raised)
    // AND to the dimmest ink rung. The previous treatment moved only the ink,
    // which made "no microphone attached" and "microphone switched off" look the
    // same. It keeps its hairline, because a source that cannot be used stays
    // VISIBLE and disabled rather than disappearing (product spec §8) — losing
    // the border as well left a blank patch of dock where a control should be.
    // Unavailable does not mean stateless. The transport locks its source
    // toggles for the whole recording, which is exactly when "is the system
    // audio actually being recorded?" matters most — so an unavailable control
    // that is ON keeps the FULL accent on its icon and its ring, and says
    // "not interactive" through the fill instead: flat with the dock, only
    // lightly tinted, and with no hover or press state.
    //
    // The accent was briefly muted here (45 % on the ring, 60 % on the icon) to
    // reinforce the unavailability. The contrast gate rejected it — the ring
    // fell to 1.9:1 against a light dock — and the fix is the right design
    // anyway: dimming the one cue that answers "which sources am I recording?"
    // was paying in legibility for something the fill already says.
    function dockFill(available: bool, active: bool, hovered: bool, pressed: bool): color {
        if (!available)
            return active ? root.accentTint(root.surface, 0.12) : root.surface;
        const base = hovered || pressed ? root.surfaceHover : root.surfaceRaised;
        const rest = active ? root.accentTint(base, 0.22) : base;
        return pressed ? root.pressTint(rest) : rest;
    }

    function dockBorder(available: bool, active: bool, hovered: bool, errorState: bool, focused: bool): color {
        if (focused)
            return root.text;
        if (!available)
            return active ? root.accent : root.line;
        if (errorState)
            return root.error;
        if (active)
            return root.accent;
        return hovered ? root.lineStrong : root.line;
    }

    function dockInk(available: bool, active: bool, errorState: bool, hovered: bool): color {
        if (!available)
            return active ? root.accent : root.textDim;
        if (errorState)
            return root.error;
        if (active)
            return root.accent;
        return hovered ? root.text : root.textSecondary;
    }

    // ── Advisory tone ────────────────────────────────────────────────────────
    //
    // The vocabulary is notifications::AdvisoryStatusForType()'s:
    // "success" | "caution" | "error" | "info". It was written out as a compare
    // chain five times across three files — the desktop toast alone carried three
    // of them, one per family below — and every copy fell through to the ACCENT,
    // the selection colour, which the canon says is never a semantic state. A
    // fifth status string would therefore have shipped as "selected" on every
    // surface at once.
    //
    // Three families because a tone answers three different questions: what it
    // marks (advisoryTone), what ink reads on a fill of it (advisoryToneInk), and
    // what colour it is as CONTENT rather than as a mark (advisoryToneText — see
    // ExoBadge for the rule).
    //
    // `info` and any unknown string share one branch on purpose: the fallback is
    // the neutral informational treatment, not the accent.
    function advisoryTone(tone: string): color {
        return tone === "success" ? root.success
             : tone === "caution" ? root.warning
             : tone === "error" ? root.error
             : root.accent;
    }

    function advisoryToneInk(tone: string): color {
        return tone === "success" ? root.successInk
             : tone === "caution" ? root.warningInk
             : tone === "error" ? root.errorInk
             : root.accentInk;
    }

    function advisoryToneText(tone: string): color {
        return tone === "success" ? root.successText
             : tone === "caution" ? root.warningText
             : tone === "error" ? root.errorText
             : root.accent;
    }

    // The same three helpers for a FIXED-DARK ground. A capture-excluded surface
    // may not resolve a tone against the application appearance -- in Light that
    // puts a light advisory ground and dark ink on a near-black card. These pick
    // the Dark appearance's semantics instead, which is the same rule the
    // overlay* colour tokens follow, and the ink deliberately stays the shared
    // on-semantic ink: it is chosen for contrast against the semantic colour, not
    // against the page behind it.
    function overlayAdvisoryTone(tone: string): color {
        return tone === "success" ? root.overlaySuccess
             : tone === "caution" ? root.overlayWarning
             : tone === "error" ? root.overlayError
             : root.overlayAccent;
    }

    function overlayAdvisoryToneInk(tone: string): color {
        return tone === "success" ? root.successInk
             : tone === "caution" ? root.warningInk
             : tone === "error" ? root.errorInk
             : root.accentInk;
    }

    function overlayAdvisoryToneText(tone: string): color {
        return tone === "success" ? root.overlaySuccess
             : tone === "caution" ? root.overlayWarning
             : tone === "error" ? root.overlayError
             : root.overlayAccent;
    }

    // QCR-513. The same tone, said without colour: a severity carried only by a
    // coloured dot is not carried at all for a user who cannot separate those
    // hues, and is carried by nothing whatsoever for a screen reader.
    //
    // Spoken severity. Prefixed to an accessible name, so it reads as
    // "Warning. Storage running low." — the same shape ExoNotice uses.
    //
    // The matching GLYPH is deliberately not here. ExoGlyph is not part of every
    // QML module that consumes this singleton (the Edit-timeline test module
    // takes ExoTheme without it), so naming it here is an unqualified access in
    // those modules — a warning against a file that never changed. The glyph
    // mapping stays with the surface that draws it, exactly as ExoNotice and
    // ExoStatusTile already keep theirs.
    function advisoryToneName(tone: string): string {
        return tone === "success" ? qsTr("Success")
             : tone === "caution" ? qsTr("Warning")
             : tone === "error" ? qsTr("Error")
             : qsTr("Information");
    }

    // ── Responsive width classes ─────────────────────────────────────────────
    //
    // A desktop recorder, not a web page: three classes are enough, and every one
    // of them maps to a real composition decision the pages already need.
    //
    //   compact  (< 1000)  — one column; the 860x700 minimum window lives here
    //   regular  (1000..)  — two independent card columns become useful
    //   wide     (1360..)  — denser grids and wider side rails pay off
    //
    // Below `rowStackWidth` a label/control pair no longer fits side by side and
    // the row stacks. That is measured per COLUMN, not per window, which is why it
    // is a separate threshold rather than a fourth class.
    readonly property int widthRegular: 1000
    readonly property int widthWide: 1360
    readonly property int rowStackWidth: 460

    // Widest the page content is allowed to get before the leftover space becomes
    // a margin instead of more content. Responsive is not the same as
    // edge-to-edge: a label pinned left and its control pinned right across 1560
    // px puts half a screen of nothing between a setting and the thing that
    // changes it, which is what the un-capped Settings page did at 1600.
    //
    // Derived by comparison, not by rule: the Widgets reference centred roughly a
    // 1040 px column at 1600. This is a little wider because Quick keeps each
    // row's hint text inline rather than behind an information tooltip.
    //
    // Logs is the deliberate exception — a log line is data, and truncating it to
    // protect a reading measure would be the wrong trade for a tool surface.
    readonly property int contentMaxWidth: 1160

    function isRegular(w: real): bool {
        return w >= root.widthRegular;
    }

    function isWide(w: real): bool {
        return w >= root.widthWide;
    }

    // Card columns for a page of width `w`, never more than `max`.
    function columnsFor(w: real, max: int): int {
        const wanted = root.isWide(w) ? 3 : root.isRegular(w) ? 2 : 1;
        return Math.min(wanted, max);
    }

    // True when a label/control pair must stack because its own column is narrow.
    function stackRows(columnWidth: real): bool {
        return columnWidth < root.rowStackWidth;
    }

    // Column count for a grid of equally-sized items that each need at least
    // `minItemWidth`. Content-driven rather than breakpoint-driven, because a tile
    // strip inside a narrow column and the same strip on a wide page want the same
    // rule, not two different window thresholds.
    function gridColumns(availableWidth: real, minItemWidth: real, gap: real, maxColumns: int): int {
        if (availableWidth <= 0 || minItemWidth <= 0)
            return 1;
        const fits = Math.floor((availableWidth + gap) / (minItemWidth + gap));
        return Math.max(1, Math.min(maxColumns, fits));
    }

    readonly property string sansFamily: sansFont.status === FontLoader.Ready ? sansFont.name : "Segoe UI"
    readonly property string monoFamily: monoFont.status === FontLoader.Ready ? monoFont.name : "Consolas"

    // Every face the family ships, not only the one the family is named after.
    // A FontLoader registers ONE file; a weight with no registered face is
    // resolved from whatever is registered, which leaves `Font.DemiBold` drawing
    // the Regular outline at Regular metrics. The product asks for Medium,
    // DemiBold and Bold in more than fifty places, so all four are loaded and
    // `sansFamily` still comes from the Regular loader — they register into one
    // family, and the family name is what the weight then selects within.
    readonly property FontLoader sansFont: FontLoader {
        source: "qrc:/fonts/HankenGrotesk-Regular.ttf"
    }

    readonly property FontLoader sansMediumFont: FontLoader {
        source: "qrc:/fonts/HankenGrotesk-Medium.ttf"
    }

    readonly property FontLoader sansSemiBoldFont: FontLoader {
        source: "qrc:/fonts/HankenGrotesk-SemiBold.ttf"
    }

    readonly property FontLoader sansBoldFont: FontLoader {
        source: "qrc:/fonts/HankenGrotesk-Bold.ttf"
    }

    readonly property FontLoader monoFont: FontLoader {
        source: "qrc:/fonts/IBMPlexMono-Regular.ttf"
    }

    // The mono family ships two faces and the product asks for a heavier one in
    // the log viewer and the diagnostics tables. It has no face above Medium, so
    // a DemiBold request there resolves to this one rather than to Regular.
    readonly property FontLoader monoMediumFont: FontLoader {
        source: "qrc:/fonts/IBMPlexMono-Medium.ttf"
    }
}
