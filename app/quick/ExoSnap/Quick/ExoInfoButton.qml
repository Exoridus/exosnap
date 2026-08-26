import QtQuick
import QtQuick.Controls.Basic

// The second of a settings row's two mutually exclusive explanation modes.
//
// A row either states its trade-off in one visible line under the label, or it
// carries this trigger and says nothing until asked. Never both: two greys under
// one label, one of them a summary of the other, is how a settings page turns
// into prose that nobody reads.
//
// A trigger rather than a bare tooltip, because the content this holds is worth
// reading deliberately -- it is the place where a technical control explains what
// it actually does. Hover still opens it (that is the cheapest way in), but the
// glyph is a real focusable control, so the explanation is reachable from the
// keyboard and announced to a screen reader, which a hover-only tooltip is not.
AbstractButton {
    id: root

    // Names what is being explained. Read out as part of the accessible name, and
    // drawn as the popover's heading, so the panel is self-identifying once it has
    // left the row it belongs to.
    required property string subject
    required property string body

    // Whether the panel is currently open. Public because the state is worth
    // observing from outside -- a test asserts the open/close contract through it
    // rather than by reaching into the popup's internals.
    readonly property alias explaining: popover.visible

    implicitWidth: 16
    implicitHeight: 16
    hoverEnabled: true
    // A bare AbstractButton is not focusable without this.
    focusPolicy: Qt.StrongFocus
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("About %1").arg(root.subject)
    Accessible.description: root.body
    // Opening on focus as well as on click made the two fight: a click focuses
    // the button first, so the panel was already open by the time `clicked`
    // arrived and the click read as "close". Focus alone no longer opens
    // anything; the body reaches a screen reader through Accessible.description
    // without it, and Space or Return still opens the panel like any button.
    onClicked: {
        if (root._pinned && popover.visible) {
            popover.close();
            return;
        }
        root._pinned = true;
        popover.open();
    }

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Escape && popover.visible) {
            event.accepted = true;
            popover.close();
        }
    }

    // Hover opens on a delay and closes as soon as the pointer leaves, unless the
    // panel was asked for deliberately -- an explanation the user clicked open
    // must not evaporate because the pointer moved a few pixels on the way to it.
    property bool _pinned: false

    onHoveredChanged: {
        if (root.hovered)
            hoverTimer.restart();
        else if (!root._pinned)
            popover.close();
    }

    HoverHandler {
        cursorShape: Qt.PointingHandCursor
    }

    Timer {
        id: hoverTimer

        interval: 350
        onTriggered: if (root.hovered) popover.open()
    }

    contentItem: ExoGlyph {
        kind: ExoGlyph.Info
        color: root.hovered || root.visualFocus || popover.visible ? ExoTheme.text : ExoTheme.textMuted
        width: 16
        height: 16
    }

    background: Rectangle {
        color: "transparent"
        border.width: root.visualFocus ? ExoTheme.focusRingWidth : 0
        border.color: ExoTheme.text
        radius: width / 2
    }

    Popup {
        id: popover

        // Anchored to the trigger, opening downward, and deliberately NOT part of
        // the row's own layout: an explanation that pushed the rows below it down
        // would reflow the page every time a pointer crossed a glyph.
        x: root.width + ExoTheme.spacingSm
        y: -ExoTheme.spacingXs
        // Wide enough for a real paragraph, narrow enough to stay a readable
        // measure (~60 characters at 13 px).
        width: 340
        padding: ExoTheme.spacingMd
        modal: false
        focus: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        onClosed: root._pinned = false

        background: Rectangle {
            color: ExoTheme.surfaceRaised
            border.width: 1
            border.color: ExoTheme.lineStrong
            radius: ExoTheme.radiusMd
        }

        contentItem: Column {
            spacing: ExoTheme.spacingXs

            Label {
                text: root.subject
                textFormat: Text.PlainText
                width: parent.width
                wrapMode: Text.WordWrap
                color: ExoTheme.text
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontBody
                    weight: Font.DemiBold
                }
            }

            Label {
                text: root.body
                textFormat: Text.PlainText
                width: parent.width
                wrapMode: Text.WordWrap
                color: ExoTheme.textSecondary
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontSecondary
                }
            }
        }
    }
}
