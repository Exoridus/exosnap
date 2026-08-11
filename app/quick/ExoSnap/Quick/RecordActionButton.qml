import QtQuick
import QtQuick.Controls.Basic

Button {
    id: root

    required property string accessibleLabel
    property color emphasisColor: ExoTheme.surfaceRaised
    property color emphasisTextColor: ExoTheme.textSecondary
    property bool round: true

    implicitWidth: root.round ? 44 : Math.max(72, contentItem.implicitWidth + 24)
    implicitHeight: 44
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.name: root.accessibleLabel

    contentItem: Label {
        text: root.text
        textFormat: Text.PlainText
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: root.enabled ? root.emphasisTextColor : ExoTheme.textDim
        font {
            family: ExoTheme.sansFamily
            pixelSize: root.round ? 16 : 13
            weight: Font.DemiBold
        }
    }

    background: Rectangle {
        color: root.down ? Qt.darker(root.emphasisColor, 1.14)
                         : root.hovered && root.enabled ? Qt.lighter(root.emphasisColor, 1.08)
                                                       : root.emphasisColor
        border.width: root.visualFocus ? 2 : 1
        border.color: root.visualFocus ? ExoTheme.text : ExoTheme.lineStrong
        radius: root.round ? height / 2 : ExoTheme.radiusSm
    }

    ToolTip.visible: root.hovered
    ToolTip.text: root.accessibleLabel
}
