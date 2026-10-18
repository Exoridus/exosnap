pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root
    required property EditPlayerAdapter player
    required property EditSessionAdapter session
    signal openMedia()
    objectName: "editViewer"
    color: ExoTheme.overlaySurface
    radius: ExoTheme.radiusMd
    border.width: 1
    border.color: ExoTheme.overlayLine
    clip: true

    ExoEditPlayerItem {
        id: surface
        objectName: "editViewerSurface"
        anchors.fill: parent
        playerAdapter: root.player
        cornerRadius: ExoTheme.radiusMd
    }
    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(440, root.width - 32)
        spacing: ExoTheme.spacingSm
        visible: root.session.durationMs === 0
        Label {
            objectName: "editEmptyViewerTitle"
            Layout.fillWidth: true
            text: qsTr("No media in the timeline yet")
            color: ExoTheme.overlayInk
            font.family: ExoTheme.sansFamily
            font.pixelSize: ExoTheme.fontBody
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Add a recording or drag a file onto the timeline.")
            color: ExoTheme.overlayInkSecondary
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
        Button {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Open media")
            onClicked: root.openMedia()
        }
    }
    Label {
        anchors.centerIn: parent
        width: Math.min(440, root.width - 32)
        visible: root.session.durationMs > 0 && !surface.hasFrame
        text: surface.errorText.length > 0 ? qsTr("Preview unavailable. Export options are checked separately.") : root.player.placeholderText
        textFormat: Text.PlainText
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        color: ExoTheme.overlayInkMuted
    }
}
