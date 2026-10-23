pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root
    required property EditPlayerAdapter player
    required property EditSessionAdapter session
    signal openMedia()
    signal chooseAudioRequested()
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
        visible: root.player.harnessStillSource.toString().length === 0
        playerAdapter: visible ? root.player : null
        cornerRadius: ExoTheme.radiusMd
    }
    Image {
        id: harnessStill
        objectName: "editHarnessStill"
        anchors.fill: parent
        source: root.player.harnessStillSource
        sourceSize: Qt.size(width, height)
        fillMode: Image.PreserveAspectFit
        visible: source.toString().length > 0
        Accessible.ignored: true
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
        ExoButton {
            objectName: "editOpenMedia"
            Layout.alignment: Qt.AlignHCenter
            leadingGlyph: ExoGlyph.Folder
            text: qsTr("Open media")
            onClicked: root.openMedia()
        }
    }
    Label {
        anchors.centerIn: parent
        width: Math.min(440, root.width - 32)
        objectName: "editViewerReason"
        visible: root.session.durationMs > 0 && !surface.hasFrame && !harnessStill.visible
        text: surface.errorText.length > 0 ? qsTr("Preview unavailable. Export options are checked separately.") : root.player.placeholderText
        textFormat: Text.PlainText
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        color: ExoTheme.overlayInkMuted
    }
    Rectangle {
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: ExoTheme.spacingSm
        width: Math.min(parent.width - 24, 440)
        height: stateLabel.implicitHeight + 16
        radius: ExoTheme.radiusSm
        color: ExoTheme.overlaySurface
        border.color: ExoTheme.overlayLine
        objectName: "editViewerStateBadge"
        visible: (surface.hasFrame || harnessStill.visible) && (root.player.seekPending || root.player.previewState === EditPlayerAdapter.Degraded || root.player.previewState === EditPlayerAdapter.Failed)
        Label {
            id: stateLabel
            anchors.fill: parent
            anchors.margins: 8
            text: root.player.seekPending ? qsTr("Preview pending") : root.player.placeholderText
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            color: ExoTheme.overlayInk
            Accessible.role: Accessible.AlertMessage
        }
    }
    ExoButton {
        objectName: "editViewerAudioChoice"
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: ExoTheme.spacingMd
        visible: root.session.sourceAudioChoices.length > 0
        text: qsTr("Choose source audio")
        onClicked: root.chooseAudioRequested()
    }
}
