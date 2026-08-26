import QtQuick
import QtQuick.Controls
import QtQuick.Shapes

// Player frame of the Edit surface: the decoded picture, a floating play/pause
// toggle, and the clip's own meta line in the corner.
//
// The picture is ExoEditPlayerItem, a scene-graph item — there is no native
// child window anywhere in this component, so the toggle and the meta line are
// ordinary QML items composited over it rather than siblings fighting a HWND.
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

    // QCR-504. This was a Rectangle with a MouseArea that CLAIMED
    // Accessible.Button: the editor's central transport control was mouse-only,
    // not in the tab order, and inert to Enter/Space. An AbstractButton keeps
    // the drawing exactly as it was and makes the claim true; the timeline
    // below adds the rest of the keyboard contract.
    AbstractButton {
        id: playToggle

        objectName: "editPlayerToggle"
        anchors.centerIn: parent
        implicitWidth: 60
        implicitHeight: 60
        // Nothing to transport without a decodable clip — and the toggle would
        // otherwise sit on top of the placeholder that says so.
        //
        // Deliberately still bound to clipOpen alone, NOT to the fade below.
        // QCR-504 made this a real control in the tab order, reachable by
        // Enter/Space; `visible: false` would take it back out of both. The
        // opacity does the hiding, and hover brings it back before any pointer
        // can reach it, so a faded toggle is never an unmarked hit target.
        visible: root.player.clipOpen
        // The picture is the subject of this surface; the transport is not. It
        // stands while the preview is paused (the surface has to say how to
        // start it), while the pointer or the keyboard is on it, and for a
        // moment after a transport change so the new state is legible before it
        // clears. A scrub hides it outright: the drag is aimed at the frame
        // under the playhead, which is exactly what an overlay would cover.
        opacity: root.player.scrubbing ? 0.0
                 : (!root.player.playing || playToggle.hovered || playToggle.visualFocus
                    || transportHold.running) ? 1.0 : 0.0
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.role: Accessible.Button
        Accessible.name: root.player.playing ? qsTr("Pause preview") : qsTr("Play preview")
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

        contentItem: Item {
            Shape {
                anchors.centerIn: parent
                width: 20
                height: 22
                visible: !root.player.playing
                preferredRendererType: Shape.CurveRenderer

                ShapePath {
                    fillColor: ExoTheme.text
                    strokeWidth: -1
                    startX: 3
                    startY: 0

                    PathLine {
                        x: 20
                        y: 11
                    }

                    PathLine {
                        x: 3
                        y: 22
                    }

                    PathLine {
                        x: 3
                        y: 0
                    }
                }
            }

            Row {
                anchors.centerIn: parent
                spacing: 6
                visible: root.player.playing

                Rectangle {
                    width: 5
                    height: 22
                    radius: 1
                    color: ExoTheme.text
                }

                Rectangle {
                    width: 5
                    height: 22
                    radius: 1
                    color: ExoTheme.text
                }
            }
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
