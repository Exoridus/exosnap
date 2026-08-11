import QtQuick
import QtQuick.Controls.Basic

// One row of an ExoMenu.
MenuItem {
    id: root

    implicitWidth: Math.max(160, contentItem.implicitWidth + leftPadding + rightPadding)
    implicitHeight: ExoTheme.controlHeight
    leftPadding: ExoTheme.spacingMd
    rightPadding: ExoTheme.spacingLg
    topPadding: 0
    bottomPadding: 0

    contentItem: Label {
        verticalAlignment: Text.AlignVCenter
        text: root.text
        textFormat: Text.PlainText
        color: root.enabled ? ExoTheme.text : ExoTheme.textDim
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontBody
        }
    }

    background: Rectangle {
        color: root.down ? ExoTheme.surfaceHover
             : root.highlighted && root.enabled ? ExoTheme.hoverTint(ExoTheme.surfaceRaised) : "transparent"
        radius: ExoTheme.radiusSm
    }

    // The Basic style draws a tick column for checkable items; nothing in this
    // product uses one, and an empty column just indents every label.
    indicator: Item {}
    arrow: Item {}
}
