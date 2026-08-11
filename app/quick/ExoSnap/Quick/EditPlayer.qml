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

    Rectangle {
        id: playToggle

        anchors.centerIn: parent
        width: 60
        height: 60
        radius: 30
        // Nothing to transport without a decodable clip — and the toggle would
        // otherwise sit on top of the placeholder that says so.
        visible: root.player.clipOpen
        color: playPointer.containsMouse ? ExoTheme.surfaceHover : ExoTheme.surface
        opacity: 0.85
        border.width: 1
        border.color: ExoTheme.lineStrong

        Accessible.role: Accessible.Button
        Accessible.name: root.player.playing ? qsTr("Pause preview") : qsTr("Play preview")
        Accessible.onPressAction: root.player.togglePlay()

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

        MouseArea {
            id: playPointer

            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.player.togglePlay()
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
