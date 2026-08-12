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
    readonly property color toneColor: root.tone === "recording" ? ExoTheme.error
                                     : root.tone === "error" ? ExoTheme.error
                                     : root.tone === "warning" ? ExoTheme.warning
                                     : root.tone === "success" ? ExoTheme.success
                                     : root.tone === "paused" ? ExoTheme.accent
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
                                     : ExoTheme.success

    // Over the preview the pill has its own near-black ground in BOTH
    // appearances — it has to, because what is behind it is arbitrary captured
    // content — so its label cannot take an appearance colour. Light's
    // `textMuted` on that ground measures 3.06:1 and Light's `error` 4.42:1,
    // both under the 4.5:1 the contrast gate holds text to; the tone is carried
    // by the dot and the ring, which are graphical and clear 3:1. A literal
    // rather than a token for the same reason OverlayRecording uses literals:
    // there is no appearance in which this ground is light.
    readonly property color onSurfaceInk: "#F1F1EF"

    implicitWidth: content.implicitWidth + 2 * (root.onSurface ? ExoTheme.spacingMd : ExoTheme.spacingXs)
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
            text: root.onSurface ? root.text.toUpperCase() : root.text
            textFormat: Text.PlainText
            elide: Text.ElideRight
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
