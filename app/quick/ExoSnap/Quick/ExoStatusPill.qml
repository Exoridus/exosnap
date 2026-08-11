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

    readonly property color toneColor: root.tone === "recording" ? ExoTheme.error
                                     : root.tone === "error" ? ExoTheme.error
                                     : root.tone === "warning" ? ExoTheme.warning
                                     : root.tone === "success" ? ExoTheme.success
                                     : root.tone === "paused" ? ExoTheme.warning
                                     : ExoTheme.success

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
            color: root.onSurface ? root.toneColor : ExoTheme.textSecondary
            font {
                family: root.onSurface ? ExoTheme.monoFamily : ExoTheme.sansFamily
                pixelSize: root.onSurface ? 10 : ExoTheme.fontSecondary
                weight: Font.DemiBold
            }
        }
    }
}
