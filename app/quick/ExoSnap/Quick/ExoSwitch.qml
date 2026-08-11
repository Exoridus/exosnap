import QtQuick
import QtQuick.Controls.Basic

Switch {
    id: root

    signal toggledByUser(bool value)

    // A switch is 20 px of glyph. Reserving a full control height around it is
    // what made every toggle row on the Settings page twice as tall as it needs
    // to be, so the compact rung is the right one here.
    implicitHeight: ExoTheme.controlHeightCompact
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    padding: 0

    onClicked: root.toggledByUser(root.checked)

    indicator: Rectangle {
        implicitWidth: 38
        implicitHeight: 20
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
            width: 14
            height: 14
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
