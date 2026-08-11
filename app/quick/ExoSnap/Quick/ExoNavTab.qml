import QtQuick
import QtQuick.Controls.Basic

// A destination in the shell's title band.
//
// Underline selection rather than an outlined pill. Both were tried; side by
// side the pill draws a permanent rounded box around one of six words, which
// reads as a button that happens to be stuck down rather than as the page you
// are on, and it competes with the window buttons at the other end of the same
// 40 px band. The underline states the same thing with one mint rule and no
// enclosing shape.
//
// The pill's one real advantage was that it could carry the keyboard focus ring.
// That is kept — but only on visualFocus, so it appears when a keyboard user
// needs it instead of permanently.
Button {
    id: root

    // Sized to its label, not to a grid: six tabs, a wordmark, a status pill, a
    // bell and three window buttons all share one 40 px band at the 860 px
    // minimum window, and a per-tab minimum width is what pushed the close
    // button off the right edge there.
    implicitWidth: contentItem.implicitWidth + leftPadding + rightPadding
    implicitHeight: 40 - 2 * ExoTheme.spacingXs
    leftPadding: ExoTheme.spacingMd
    rightPadding: ExoTheme.spacingMd
    topPadding: 0
    bottomPadding: 0
    hoverEnabled: true
    checkable: true
    autoExclusive: true
    focusPolicy: Qt.StrongFocus

    property alias selected: root.checked

    contentItem: Label {
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        text: root.text
        textFormat: Text.PlainText
        color: root.selected ? ExoTheme.text : root.hovered ? ExoTheme.text : ExoTheme.textSecondary
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontBody
            // The selected tab is the only bold one, so the weight change is the
            // second selection cue after the rule below.
            weight: root.selected ? Font.DemiBold : Font.Medium
        }
    }

    background: Item {
        Rectangle {
            anchors.fill: parent
            color: root.down ? ExoTheme.surfaceHover : ExoTheme.surfaceRaised
            radius: ExoTheme.radiusSm
            visible: root.hovered && !root.selected || root.down
        }

        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.width: ExoTheme.focusRingWidth
            border.color: ExoTheme.text
            radius: ExoTheme.radiusSm
            visible: root.visualFocus
        }

        Rectangle {
            height: 2
            color: ExoTheme.accent
            radius: 1
            visible: root.selected
            anchors {
                left: parent.left
                right: parent.right
                bottom: parent.bottom
                leftMargin: ExoTheme.spacingXs
                rightMargin: ExoTheme.spacingXs
                bottomMargin: -ExoTheme.spacingXs
            }
        }
    }
}
