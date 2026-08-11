import QtQuick
import QtQuick.Controls.Basic

TextField {
    id: root

    signal committed(string value)

    implicitHeight: ExoTheme.controlHeight
    selectByMouse: true
    color: root.enabled ? ExoTheme.text : ExoTheme.textDim
    placeholderTextColor: ExoTheme.textDim
    selectionColor: ExoTheme.accent
    selectedTextColor: ExoTheme.accentInk
    leftPadding: ExoTheme.spacingMd
    rightPadding: ExoTheme.spacingMd
    font {
        family: ExoTheme.sansFamily
        pixelSize: ExoTheme.fontBody
    }

    background: Rectangle {
        color: root.enabled ? ExoTheme.surfaceRaised : ExoTheme.surface
        border.width: 1
        border.color: root.activeFocus ? ExoTheme.accent : ExoTheme.line
        radius: ExoTheme.radiusSm
    }

    onEditingFinished: root.committed(root.text)
}
