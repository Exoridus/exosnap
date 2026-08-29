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

    // The mapping itself lives in ExoTheme.toneColor, which the Record stage's
    // border reads too.
    readonly property color toneColor: ExoTheme.toneColor(root.tone, root.onSurface)

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
