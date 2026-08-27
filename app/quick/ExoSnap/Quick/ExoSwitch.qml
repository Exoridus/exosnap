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

    // A disabled control keeps the arrow, the same rule ExoButton states: the
    // hand says "press this", and a blocked switch has nothing to press.
    HoverHandler {
        cursorShape: root.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
    }

    // Blocked is a state of its own, not the absence of one.
    //
    // A switch that is disabled while ON still has to answer "is this setting
    // on?" -- that is exactly the question a locked recording raises, and the
    // previous treatment answered it wrong: the ON fill was dropped for the plain
    // recessed surface, so a locked-on switch read as off with its knob parked on
    // the right. It is the same trade the transport dock already makes for an
    // unavailable source (see ExoTheme.dockFill): keep the accent, say "not
    // interactive" through a flat fill, a hairline border and a dim knob, and
    // drop hover entirely.
    readonly property color _trackColor: !root.enabled
                                         ? (root.checked ? ExoTheme.accentTint(ExoTheme.surface, 0.22)
                                                         : ExoTheme.surface)
                                         : root.checked ? (root.hovered ? ExoTheme.hoverTint(ExoTheme.accent)
                                                                        : ExoTheme.accent)
                                                        : (root.hovered ? ExoTheme.hoverTint(ExoTheme.surfaceHover)
                                                                        : ExoTheme.surfaceHover)

    readonly property color _borderColor: root.visualFocus ? ExoTheme.text
                                          : !root.enabled ? (root.checked ? ExoTheme.accent : ExoTheme.line)
                                          : root.checked ? ExoTheme.accent : ExoTheme.lineStrong

    // The knob carries the interactivity. Full-strength ink on a live control,
    // the dimmest rung on a blocked one, in both checked states.
    readonly property color _knobColor: !root.enabled ? ExoTheme.textDim
                                        : root.checked ? ExoTheme.accentInk : ExoTheme.textSecondary

    indicator: Rectangle {
        implicitWidth: 44
        implicitHeight: 24
        x: root.leftPadding
        y: root.topPadding + (root.availableHeight - height) / 2
        color: root._trackColor
        border.width: 1
        border.color: root._borderColor
        radius: height / 2

        Rectangle {
            x: root.checked ? parent.width - width - 3 : 3
            width: 18
            height: 18
            color: root._knobColor
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
