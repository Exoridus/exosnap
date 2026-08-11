import QtQuick
import QtQuick.Controls.Basic

CheckBox {
    id: root

    signal toggledByUser(bool value)

    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    padding: 0

    indicator: Rectangle {
        x: root.leftPadding
        y: root.topPadding + (root.availableHeight - height) / 2
        implicitWidth: 18
        implicitHeight: 18
        color: !root.enabled ? ExoTheme.surface
                             : root.checked ? (root.hovered ? ExoTheme.hoverTint(ExoTheme.accent) : ExoTheme.accent)
                                            : (root.hovered ? ExoTheme.hoverTint(ExoTheme.surfaceHover) : ExoTheme.surfaceHover)
        border.width: 1
        border.color: root.visualFocus ? ExoTheme.text
                                       : root.checked ? ExoTheme.accent : ExoTheme.lineStrong
        radius: ExoTheme.radiusSm - 4

        Label {
            text: "✓"
            textFormat: Text.PlainText
            visible: root.checked
            anchors.centerIn: parent
            color: root.enabled ? ExoTheme.accentInk : ExoTheme.textDim
            font {
                family: ExoTheme.sansFamily
                pixelSize: 12
                weight: Font.DemiBold
            }
        }
    }

    contentItem: Label {
        text: root.text
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignVCenter
        visible: root.text !== ""
        color: root.enabled ? ExoTheme.textSecondary : ExoTheme.textDim
        leftPadding: root.indicator.width + ExoTheme.spacingXs
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontSecondary
        }
    }

    onClicked: root.toggledByUser(root.checked)
}
