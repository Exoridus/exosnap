import QtQuick
import QtQuick.Controls.Basic

Switch {
    id: root

    signal toggledByUser(bool value)

    // A switch does not need a full control height of surrounding air, but it
    // does need to be hittable and readable: shrunk to a 38x20 glyph on a 26 px
    // row it read as a decoration rather than as the control that decides
    // whether a recording captures your microphone.
    implicitHeight: ExoTheme.controlHeightCompact
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    padding: 0

    onClicked: root.toggledByUser(root.checked)

    indicator: Rectangle {
        implicitWidth: 44
        implicitHeight: 24
        x: root.leftPadding
        y: root.topPadding + (root.availableHeight - height) / 2
        color: !root.enabled ? ExoTheme.surface
                             : root.checked ? (root.hovered ? ExoTheme.hoverTint(ExoTheme.accent) : ExoTheme.accent)
                                            : (root.hovered ? ExoTheme.hoverTint(ExoTheme.surfaceHover) : ExoTheme.surfaceHover)
        border.width: 1
        border.color: root.visualFocus ? ExoTheme.text : root.checked && root.enabled ? ExoTheme.accent : ExoTheme.lineStrong
        radius: height / 2

        Rectangle {
            x: root.checked ? parent.width - width - 3 : 3
            width: 18
            height: 18
            color: root.checked && root.enabled ? ExoTheme.accentInk : ExoTheme.textSecondary
            radius: height / 2
            anchors.verticalCenter: parent.verticalCenter

            Behavior on x {
                XAnimator {
                    duration: ExoTheme.animFast
                    easing.type: Easing.OutCubic
                }
            }
        }
    }

    contentItem: Label {
        text: root.text
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignVCenter
        visible: root.text !== ""
        color: root.enabled ? ExoTheme.text : ExoTheme.textDim
        leftPadding: root.indicator.width + ExoTheme.spacingSm
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontBody
        }
    }
}
