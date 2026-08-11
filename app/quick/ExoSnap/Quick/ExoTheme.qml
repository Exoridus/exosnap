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
    readonly property int rowSpacing: 9        // between setting rows inside a card
    readonly property int sectionGap: 16       // between cards on a page
    readonly property int cardPadding: 15      // a card's own inset
    readonly property int cardPaddingCompact: 11
    readonly property int pagePadding: 20      // page edge inset

    // ── Radii ────────────────────────────────────────────────────────────────
    readonly property int radiusSm: 8
    readonly property int radiusMd: 10
    readonly property int radiusLg: 14
    readonly property int radiusPill: 999

    // ── Control sizing ───────────────────────────────────────────────────────
    readonly property int controlHeight: 32
    readonly property int controlHeightCompact: 26
    readonly property int controlHeightLarge: 40

    // ── Typography ───────────────────────────────────────────────────────────
    // Semantic rungs, not a numeric scale: the caller names the role and the theme
    // decides the size, which is what keeps one page from inventing a fourth
    // heading size.
    readonly property int fontCaption: 11      // hints, units, secondary metadata
    readonly property int fontSecondary: 12    // subtitles, muted supporting text
    readonly property int fontBody: 13         // labels, controls, list rows
    readonly property int fontSectionTitle: 14 // card / section headings
    readonly property int fontBrand: 15        // the shell's wordmark
    readonly property int fontPageTitle: 19    // one per page, at most

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

    // Widest a single reading column is allowed to get. A page whose content is
    // genuinely short (a healthy Diagnostics run says almost nothing, and that is
    // the point) should not stretch its few rows across a 4K desktop; capping the
    // column turns the leftover space into a margin instead of a void. Pages that
    // fill their width — Settings' two columns, the Logs list — do not use this.
    readonly property int contentMaxWidth: 1280

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

    readonly property FontLoader sansFont: FontLoader {
        source: "qrc:/fonts/HankenGrotesk-Regular.ttf"
    }

    readonly property FontLoader monoFont: FontLoader {
        source: "qrc:/fonts/IBMPlexMono-Regular.ttf"
    }
}
