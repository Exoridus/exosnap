pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

FocusScope {
    id: root
    required property EditSessionAdapter session
    required property EditPlayerAdapter player
    property real pixelsPerSecond: 40
    property bool snapping: true
    property var visibleClips: []
    function refreshClips(): void {
        root.visibleClips = root.session.visibleClips(Math.max(0, (scroll.contentX - 50) / root.pixelsPerMs),
                                                     (scroll.contentX + scroll.width) / root.pixelsPerMs);
    }
    onPixelsPerSecondChanged: refreshClips()
    Component.onCompleted: refreshClips()
    readonly property real pixelsPerMs: pixelsPerSecond / 1000
    objectName: "editClipTimeline"
    activeFocusOnTab: true
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Edit timeline. Left and Right seek; Up and Down select clips. Alt+Left/Right moves; add Control to trim the start or Shift to trim the end. Control+B splits; Delete removes; Shift+Delete closes the gap.")
    Keys.onPressed: event => {
        const control = (event.modifiers & Qt.ControlModifier) !== 0;
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0;
        const alt = (event.modifiers & Qt.AltModifier) !== 0;
        const direction = event.key === Qt.Key_Left ? -1 : 1;
        if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
            root.session.selectAdjacentClip(event.key === Qt.Key_Up ? -1 : 1);
            event.accepted = true;
            return;
        }
        if (alt && (event.key === Qt.Key_Left || event.key === Qt.Key_Right)) {
            root.session.nudgeSelected(direction * 100, control ? 1 : shift ? 2 : 0);
            event.accepted = true;
            return;
        }
        if (control && event.key === Qt.Key_B) root.session.splitSelected();
        else if (control && event.key === Qt.Key_Z) { if (shift) root.session.redo(); else root.session.undo(); }
        else if (control && event.key === Qt.Key_Y) root.session.redo();
        else if (event.key === Qt.Key_Home) root.session.requestSeek(0);
        else if (event.key === Qt.Key_End) root.session.requestSeek(root.session.durationMs);
        else if (event.key === Qt.Key_Delete) root.session.deleteSelected(shift);
        else if (event.key === Qt.Key_Space) root.player.togglePlay();
        else if (event.key === Qt.Key_Left || event.key === Qt.Key_Right)
            root.session.requestSeek(root.session.positionMs + (event.key === Qt.Key_Left ? -1 : 1) * (shift ? 1000 : 33));
        else return;
        event.accepted = true;
    }
    Connections {
        target: root.session
        function onWorkspaceChanged(): void { root.refreshClips(); }
    }
    Rectangle {
        anchors.fill: parent
        color: ExoTheme.surface
        border.color: root.activeFocus ? ExoTheme.text : ExoTheme.line
    }
    Flickable {
        id: scroll
        anchors.fill: parent
        anchors.margins: 2
        onContentXChanged: root.refreshClips()
        onWidthChanged: root.refreshClips()
        contentWidth: Math.max(width, 60 + (root.session.durationMs + 10000) * root.pixelsPerMs)
        contentHeight: Math.max(height, 30 + root.session.tracks.length * 64)
        clip: true
        ScrollBar.horizontal: ScrollBar {}
        ScrollBar.vertical: ScrollBar {}
        MouseArea {
            width: scroll.contentWidth
            height: scroll.contentHeight
            onPressed: mouse => {
                root.forceActiveFocus();
                root.player.beginScrub();
                root.session.requestSeek(Math.max(0, (mouse.x - 50) / root.pixelsPerMs));
            }
            onPositionChanged: mouse => {
                if (pressed) root.session.requestSeek(Math.max(0, (mouse.x - 50) / root.pixelsPerMs));
            }
            onReleased: root.player.endScrub()
            onCanceled: root.player.endScrub()
        }
        Repeater {
            model: root.session.tracks
            delegate: Label {
                required property var modelData
                required property int index
                x: scroll.contentX + 4
                y: 36 + index * 64
                z: 3
                text: modelData.name
                color: ExoTheme.textSecondary
            }
        }
        Repeater {
            model: root.visibleClips
            delegate: Rectangle {
                id: clipItem
                required property var modelData
                property real dragMs: 0
                x: 50 + (modelData.startMs + dragMs) * root.pixelsPerMs
                y: 30 + modelData.trackIndex * 64
                width: Math.max(4, modelData.durationMs * root.pixelsPerMs)
                height: 56
                radius: ExoTheme.radiusSm
                color: modelData.selected ? ExoTheme.surfaceHover : ExoTheme.surfaceRaised
                border.color: modelData.selected ? ExoTheme.text : ExoTheme.lineStrong
                clip: true
                Accessible.role: Accessible.Button
                Accessible.name: modelData.name
                Accessible.onPressAction: root.session.selectClip(modelData.id)
                Label {
                    anchors.fill: parent
                    anchors.margins: 10
                    text: clipItem.modelData.name + (clipItem.modelData.available ? "" : "\n" + qsTr("Media unavailable"))
                    color: ExoTheme.text
                    elide: Text.ElideRight
                }
                MouseArea {
                    preventStealing: true
                    anchors.fill: parent
                    property real startX: 0
                    onPressed: mouse => {
                        startX = mapToItem(scroll.contentItem, mouse.x, 0).x;
                        root.forceActiveFocus();
                    }
                    onPositionChanged: mouse => {
                        if (pressed) clipItem.dragMs = (mapToItem(scroll.contentItem, mouse.x, 0).x - startX) / root.pixelsPerMs;
                    }
                    onCanceled: clipItem.dragMs = 0
                    onReleased: mouse => {
                        const delta = clipItem.dragMs;
                        clipItem.dragMs = 0;
                        if (Math.abs(delta) > 20) root.session.moveClip(clipItem.modelData.id, clipItem.modelData.startMs + delta, root.snapping);
                        else root.session.selectClip(clipItem.modelData.id);
                    }
                }
                Repeater {
                    model: 2
                    delegate: MouseArea {
                        preventStealing: true
                        required property int index
                        property real startX: 0
                        x: index === 0 ? 0 : clipItem.width - width
                        width: 8
                        height: clipItem.height
                        cursorShape: Qt.SizeHorCursor
                        onPressed: mouse => { startX = mapToItem(scroll.contentItem, mouse.x, 0).x; }
                        onReleased: mouse => {
                            const delta = (mapToItem(scroll.contentItem, mouse.x, 0).x - startX) / root.pixelsPerMs;
                            root.session.trimClip(clipItem.modelData.id, clipItem.modelData.inMs + (index === 0 ? delta : 0),
                                                  clipItem.modelData.outMs + (index === 1 ? delta : 0));
                        }
                    }
                }
            }
        }
        Rectangle {
            x: 50 + root.session.positionMs * root.pixelsPerMs
            width: 2
            height: scroll.contentHeight
            color: ExoTheme.text
        }
        DropArea {
            width: scroll.contentWidth
            height: scroll.contentHeight
            onDropped: drop => {
                const at = Math.max(0, (drop.x - 50) / root.pixelsPerMs);
                if (drop.formats.indexOf("application/x-exosnap-history") >= 0)
                    root.session.addHistory(drop.getDataAsString("application/x-exosnap-history"), at);
                else if (drop.formats.indexOf("application/x-exosnap-asset") >= 0)
                    root.session.appendAsset(Number(drop.getDataAsString("application/x-exosnap-asset")), at);
                else if (drop.hasUrls)
                    root.session.importMediaBatch(drop.urls, true, at);
                drop.acceptProposedAction();
            }
        }
    }
}
