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

    readonly property var clipDetails: session.selectedClipDetails
    readonly property var videoValues: session.videoProperties(clipDetails.id || 0, session.positionUs)
    activeFocusOnTab: true
    Keys.onPressed: event => {
        if (event.key === Qt.Key_K && event.modifiers === Qt.NoModifier && !event.isAutoRepeat) {
            root.session.autoKey = !root.session.autoKey;
            event.accepted = true;
        } else if (event.key === Qt.Key_Escape && root.session.dragActive) {
            root.session.cancelClipDrag();
            event.accepted = true;
        }
    }
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
    Item {
        id: outputBox
        anchors.centerIn: parent
        width: Math.min(root.width, root.height * (root.clipDetails.outputAspect || 1))
        height: width / (root.clipDetails.outputAspect || 1)
        visible: (root.clipDetails.video ?? false) && root.session.selectedClipIds.length === 1 && root.session.positionUs >= (root.clipDetails.startUs || 0) && root.session.positionUs < (root.clipDetails.endUs || 0)
        enabled: !root.session.trackLocked(root.clipDetails.trackId || 0)
        Item {
            id: transformBox
            readonly property real baseWidth: Math.min(outputBox.width, outputBox.height * (root.clipDetails.sourceAspect || 1))
            readonly property real baseHeight: baseWidth / (root.clipDetails.sourceAspect || 1)
            property real draftX: 0
            property real draftY: 0
            property real draftScale: 1
            property real draftRotation: 0
            readonly property bool dragging: transformMouse.pressed && root.session.videoPreviewActive
            width: baseWidth * (dragging ? draftScale : (root.videoValues[2]?.value ?? 1))
            height: baseHeight * (dragging ? draftScale : (root.videoValues[2]?.value ?? 1))
            x: (outputBox.width - width) / 2 + outputBox.width * (dragging ? draftX : (root.videoValues[0]?.value ?? 0))
            y: (outputBox.height - height) / 2 + outputBox.height * (dragging ? draftY : (root.videoValues[1]?.value ?? 0))
            rotation: dragging ? draftRotation : (root.videoValues[3]?.value ?? 0)
            Rectangle { anchors.fill: parent; color: "transparent"; border.color: ExoTheme.accent; border.width: root.activeFocus ? 2 : 1; Accessible.ignored: true }
            Rectangle { width: 12; height: 12; anchors.right: parent.right; anchors.bottom: parent.bottom; color: ExoTheme.accent; Accessible.ignored: true }
            Rectangle { width: 12; height: 12; radius: 6; anchors.right: parent.right; anchors.top: parent.top; color: ExoTheme.accent; Accessible.ignored: true }
            MouseArea {
                id: transformMouse
                anchors.fill: parent
                cursorShape: Qt.SizeAllCursor
                property point origin
                property var initial: []
                property int operation: 0
                onPressed: mouse => {
                    root.forceActiveFocus();
                    root.player.setPlaying(false);
                    initial = root.videoValues.map(row => row.value);
                    origin = mapToItem(outputBox, mouse.x, mouse.y);
                    operation = mouse.x > width - 24 ? (mouse.y < 24 ? 2 : mouse.y > height - 24 ? 1 : 0) : 0;
                    transformBox.draftX = initial[0];
                    transformBox.draftY = initial[1];
                    transformBox.draftScale = initial[2];
                    transformBox.draftRotation = initial[3];
                    mouse.accepted = root.session.beginVideoTransformDrag(root.clipDetails.id || 0);
                }
                onPositionChanged: mouse => {
                    if (!pressed || !root.session.dragActive)
                        return;
                    const point = mapToItem(outputBox, mouse.x, mouse.y);
                    if (operation === 0) {
                        transformBox.draftX = initial[0] + (point.x - origin.x) / outputBox.width;
                        transformBox.draftY = initial[1] + (point.y - origin.y) / outputBox.height;
                    } else {
                        const centerX = outputBox.width * (0.5 + initial[0]);
                        const centerY = outputBox.height * (0.5 + initial[1]);
                        if (operation === 1)
                            transformBox.draftScale = initial[2] * Math.hypot(point.x - centerX, point.y - centerY) / Math.max(1, Math.hypot(origin.x - centerX, origin.y - centerY));
                        else
                            transformBox.draftRotation = initial[3] + (Math.atan2(point.y - centerY, point.x - centerX) - Math.atan2(origin.y - centerY, origin.x - centerX)) * 180 / Math.PI;
                    }
                    root.session.updateVideoTransformDrag(transformBox.draftX, transformBox.draftY, transformBox.draftScale, transformBox.draftRotation);
                }
                onReleased: if (root.session.dragActive) root.session.commitClipDrag()
                onCanceled: root.session.cancelClipDrag()
            }
        }
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
