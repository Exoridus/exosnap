import QtQuick
import QtQuick.Controls.Basic

// Scene-graph video keeps playback controls composited over the decoded frame.
Rectangle {
    id: root

    required property EditPlayerAdapter player
    required property EditSessionAdapter session

    color: ExoTheme.surfaceRaised
    border.width: 1
    border.color: ExoTheme.line
    radius: ExoTheme.radiusLg
    clip: true

    gradient: Gradient {
        GradientStop {
            position: 0.0
            color: ExoTheme.surfaceRaised
        }

        GradientStop {
            position: 1.0
            color: ExoTheme.background
        }
    }

    ExoEditPlayerItem {
        id: surface

        anchors {
            fill: parent
            margins: ExoTheme.spacingSm
        }
        playerAdapter: root.player
        cornerRadius: ExoTheme.radiusMd
    }

    Label {
        anchors.centerIn: parent
        text: surface.errorText !== "" ? surface.errorText : root.player.placeholderText
        textFormat: Text.PlainText
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        visible: !surface.hasFrame && (surface.errorText !== "" || root.player.placeholderText !== "")
        width: parent.width - 2 * ExoTheme.spacingXl
        color: ExoTheme.textMuted
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontBody
        }
    }

    AbstractButton {
        id: playToggle

        objectName: "editPlayerToggle"
        anchors.centerIn: parent
        implicitWidth: 60
        implicitHeight: 60
        visible: root.player.clipOpen
        // Keep the faded control focusable; keyboard focus restores its visible affordance.
        opacity: root.player.scrubbing ? 0.0
                 : (!root.player.playing || playToggle.hovered || playToggle.visualFocus
                    || transportHold.running) ? 1.0 : 0.0
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.role: Accessible.Button
        Accessible.name: root.player.playing ? qsTr("Pause preview") : qsTr("Play preview")
        ToolTip.text: root.player.playing ? qsTr("Pause preview (Space)") : qsTr("Play preview (Space)")
        ToolTip.visible: hovered || visualFocus
        onClicked: root.player.togglePlay()

        Behavior on opacity {
            NumberAnimation {
                duration: ExoTheme.animMedium
                easing.type: Easing.OutCubic
            }
        }

        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }

        // The confirmation half of the contract: pressing play swaps the glyph
        // to pause, and that swap has to be seen before the toggle clears.
        Timer {
            id: transportHold

            interval: 900
        }

        Connections {
            target: root.player

            function onPlayingChanged(): void {
                // Not for the pause and resume a scrub performs on its own. The
                // adapter clears `scrubbing` only after that resume, so this
                // sees the drag still in progress and stays quiet — otherwise
                // every scrub of a playing clip ended in a pause glyph fading
                // out over the frame the user just went looking for.
                if (!root.player.scrubbing)
                    transportHold.restart();
            }
        }

        background: Rectangle {
            radius: 30
            color: playToggle.hovered ? ExoTheme.surfaceHover : ExoTheme.surface
            opacity: 0.85
            // The focus ring is the same `text` hairline every other control
            // uses; at rest the toggle keeps its own quiet boundary.
            border.width: playToggle.visualFocus ? 2 : 1
            border.color: playToggle.visualFocus ? ExoTheme.text : ExoTheme.lineStrong
        }

        contentItem: ExoGlyph {
            kind: root.player.playing ? ExoGlyph.Pause : ExoGlyph.Run
            color: ExoTheme.text
            width: 24
            height: 24
            scale: 0.5
        }
    }

    Label {
        anchors {
            right: parent.right
            bottom: parent.bottom
            margins: ExoTheme.spacingMd
        }
        text: root.session.playerMetaText
        textFormat: Text.PlainText
        color: ExoTheme.textDim
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontCaption
        }
    }
}
