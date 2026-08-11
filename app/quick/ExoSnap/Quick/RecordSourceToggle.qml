import QtQuick
import QtQuick.Controls.Basic

Button {
    id: root

    required property string shortLabel
    required property string accessibleLabel
    property alias checkedState: root.checked
    property bool errorState: false
    property real meterLevel: 0

    implicitWidth: 48
    implicitHeight: 44
    text: root.shortLabel
    hoverEnabled: true
    checkable: true
    focusPolicy: Qt.StrongFocus
    Accessible.name: root.accessibleLabel

    contentItem: Label {
        text: root.text
        textFormat: Text.PlainText
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: !root.enabled ? ExoTheme.textDim
                             : root.errorState ? ExoTheme.error
                                               : root.checkedState ? ExoTheme.accent : ExoTheme.textSecondary
        font {
            family: ExoTheme.monoFamily
            pixelSize: 10
            weight: Font.DemiBold
        }
    }

    background: Rectangle {
        color: root.down ? ExoTheme.surfaceHover : root.hovered ? ExoTheme.surfaceRaised : ExoTheme.surface
        border.width: root.visualFocus || root.checkedState || root.errorState ? 1 : 0
        border.color: root.visualFocus ? ExoTheme.text
                                       : root.errorState ? ExoTheme.error
                                                         : root.checkedState ? ExoTheme.accent : ExoTheme.line
        radius: ExoTheme.radiusSm

        Rectangle {
            height: 2
            width: Math.max(0, (parent.width - 10) * root.meterLevel)
            color: root.checkedState ? ExoTheme.accent : ExoTheme.textDim
            radius: 1
            anchors {
                left: parent.left
                bottom: parent.bottom
                leftMargin: 5
                bottomMargin: 4
            }
        }
    }

    ToolTip.visible: root.hovered
    ToolTip.text: root.accessibleLabel
}
