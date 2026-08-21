import QtQuick
import QtQuick.Controls

// The recording state, said the same way everywhere it is said: the shell's
// title bar, the badge over the live preview, and any surface that needs to
// state what the engine is doing right now.
//
// `tone` takes RecordViewModelAdapter::stateTone verbatim, so the mapping from
// engine state to colour exists once rather than once per surface.
Rectangle {
    id: root

    required property string text
    property string tone: "neutral"
    // Over a live preview the pill needs its own ground to stay legible against
    // arbitrary desktop content. In the title bar it must not draw a second box
    // inside the band.
    property bool onSurface: false

    // Three semantic tones (recording / error / warning), one success tone, and
    // two that are deliberately NOT semantic: a paused session takes the accent
    // its Resume action carries, and a momentary transition takes a quiet
    // neutral. Caution amber stays with `warning`, which is the only tone a real
    // warning uses.
    readonly property color toneColor: root.tone === "recording" ? root._error
                                     : root.tone === "error" ? root._error
                                     : root.tone === "warning" ? root._warning
                                     : root.tone === "success" ? root._success
                                     : root.tone === "paused" ? root._accent
                                     // The one tone that has to resolve per
                                     // ground. `busy` is deliberately a quiet
                                     // neutral, and Light has no LIGHT neutral —
                                     // `textMuted` there measures 2.998:1 as a
                                     // dot on the pill's near-black ground,
                                     // which the contrast gate rejects (and
                                     // rejected, before this line existed). The
                                     // semantic tones need no such split because
                                     // they are designed to read on both.
                                     : root.tone === "busy" ? (root.onSurface ? root.onSurfaceInk : ExoTheme.textMuted)
                                     : root._success

    // Over the preview the ground is near-black in BOTH appearances, so every
    // tone resolves against the Dark appearance there; in the title band the
    // ground is an appearance surface and the appearance tones are right.
    readonly property color _error: root.onSurface ? ExoTheme.overlayError : ExoTheme.error
    readonly property color _warning: root.onSurface ? ExoTheme.overlayWarning : ExoTheme.warning
    readonly property color _success: root.onSurface ? ExoTheme.overlaySuccess : ExoTheme.success
    readonly property color _accent: root.onSurface ? ExoTheme.overlayAccent : ExoTheme.accent

    // Over the preview the pill has its own near-black ground in BOTH
    // appearances — it has to, because what is behind it is arbitrary captured
    // content — so its label cannot take an appearance colour. Light's
    // `textMuted` on that ground measures 3.06:1 and Light's `error` 4.42:1,
    // both under the 4.5:1 the contrast gate holds text to.
    //
    // This was the first surface in the product to need that, and it solved it
    // with a literal. `ExoTheme.overlayInk` is that same value, named: the Dark
    // appearance's own ink, which is what a fixed-dark ground takes everywhere
    // now (the desktop overlays, the live-metrics readout, this pill).
    readonly property color onSurfaceInk: ExoTheme.overlayInk

    // Derived from the label's OWN implicit width, not from the Row's — the Row
    // measures its children's actual widths, and the label's actual width is
    // constrained below by this very number. Reading it back here would be a
    // binding loop; reading the unconstrained implicitWidth of the text is what
    // makes "as wide as the text wants" and "no wider than the pill was given"
    // two separate facts.
    readonly property int insetX: root.onSurface ? ExoTheme.spacingMd : ExoTheme.spacingXs
    readonly property int chromeWidth: 7 + content.spacing + 2 * root.insetX

    // What is left for the text after the dot, the gap and the insets. The pill
    // only elides when it has actually been given less room than it asked for,
    // which is what `elide` on an unconstrained Label never noticed: a Label
    // with no width limit is exactly as wide as its text, so ElideRight had
    // nothing to elide and the pill simply grew — in the title band, past the
    // navigation tabs it was pushing aside.
    readonly property int availableTextWidth: Math.max(0, root.width - root.chromeWidth)

    // Measured off the string rather than read back from the Label. The Label's
    // own implicit width is a live, rounded-down quantity that the layout then
    // feeds back in as this item's width — one pixel short of the text is enough
    // for ElideRight to drop three characters, and "Ready" became "Rea..." at
    // every window width, not only narrow ones. TextMetrics answers the same
    // question about the full string without participating in the layout.
    TextMetrics {
        id: labelMetrics

        text: label.text
        font: label.font
    }

    implicitWidth: Math.ceil(labelMetrics.advanceWidth) + root.chromeWidth
    implicitHeight: root.onSurface ? 26 : 20
    color: root.onSurface ? Qt.rgba(0, 0, 0, 0.72) : "transparent"
    border.width: root.onSurface ? 1 : 0
    border.color: root.toneColor
    radius: ExoTheme.radiusSm

    Accessible.role: Accessible.StaticText
    Accessible.name: root.text

    Row {
        id: content

        spacing: ExoTheme.spacingXs + 2
        anchors.centerIn: parent

        Rectangle {
            width: 7
            height: 7
            radius: 3.5
            color: root.toneColor
            anchors.verticalCenter: parent.verticalCenter
        }

        Label {
            id: label

            text: root.onSurface ? root.text.toUpperCase() : root.text
            textFormat: Text.PlainText
            elide: Text.ElideRight
            // The whole point: a real, finite width. Capped at what the pill was
            // given, never wider than the text needs. The full string stays in
            // the pill's accessible name, so nothing is lost to a screen reader.
            width: Math.min(Math.ceil(labelMetrics.advanceWidth), root.availableTextWidth)
            anchors.verticalCenter: parent.verticalCenter
            color: root.onSurface ? root.onSurfaceInk : ExoTheme.textSecondary
            font {
                family: root.onSurface ? ExoTheme.monoFamily : ExoTheme.sansFamily
                pixelSize: root.onSurface ? ExoTheme.fontEyebrow : ExoTheme.fontSecondary
                weight: Font.DemiBold
            }
        }
    }
}
