import QtQuick
import QtQuick.Controls.Basic

// A text link that is a real button: in the Tab order, activated by Space like
// every other button, with a focus ring. A Text with a TapHandler looks the same
// and is unreachable without a mouse.
AbstractButton {
    id: root

    property bool mono: false
    property bool underline: false
    // An optional glyph after the text, such as the arrow of "Show in log".
    property int trailingGlyph: ExoGlyph.Invalid

    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    padding: 2
    Accessible.role: Accessible.Link
    Accessible.name: root.text

    HoverHandler {
        cursorShape: Qt.PointingHandCursor
    }

    background: Rectangle {
        radius: ExoTheme.radiusXs
        color: "transparent"
        border.width: root.visualFocus ? ExoTheme.focusRingWidth : 0
        border.color: ExoTheme.text
    }

    contentItem: Row {
        spacing: ExoTheme.spacingXs

        Text {
            text: root.text
            textFormat: Text.PlainText
            color: root.hovered ? ExoTheme.text : ExoTheme.accent
            anchors.verticalCenter: parent.verticalCenter
            font {
                family: root.mono ? ExoTheme.monoFamily : ExoTheme.sansFamily
                pixelSize: root.mono ? ExoTheme.fontCaption : ExoTheme.fontSecondary
                weight: root.mono ? Font.Normal : Font.DemiBold
                underline: root.underline
            }
        }

        ExoGlyph {
            kind: root.trailingGlyph
            visible: root.trailingGlyph !== ExoGlyph.Invalid
            color: root.hovered ? ExoTheme.text : ExoTheme.accent
            width: ExoTheme.fontSecondary
            height: ExoTheme.fontSecondary
            anchors.verticalCenter: parent.verticalCenter
        }
    }
}
