import QtQuick
import QtQuick.Controls.Basic

Slider {
    id: root

    signal movedByUser(real value)

    implicitHeight: ExoTheme.controlHeightCompact
    hoverEnabled: true

    background: Rectangle {
        x: root.leftPadding
        y: root.topPadding + (root.availableHeight - height) / 2
        implicitWidth: 120
        implicitHeight: 4
        width: root.availableWidth
        height: implicitHeight
        color: ExoTheme.surfaceHover
        radius: height / 2

        Rectangle {
            width: root.visualPosition * parent.width
            height: parent.height
            color: root.enabled ? ExoTheme.accent : ExoTheme.textDim
            radius: height / 2
        }
    }

    handle: Rectangle {
        x: root.leftPadding + root.visualPosition * (root.availableWidth - width)
        y: root.topPadding + (root.availableHeight - height) / 2
        implicitWidth: 16
        implicitHeight: 16
        color: !root.enabled ? ExoTheme.textDim
                             : root.pressed ? ExoTheme.surfaceHover
                             : root.hovered ? ExoTheme.hoverTint(ExoTheme.text) : ExoTheme.text
        border.width: 1
        border.color: root.visualFocus ? ExoTheme.accent : ExoTheme.lineStrong
        radius: height / 2
    }

    onMoved: root.movedByUser(root.value)
}
