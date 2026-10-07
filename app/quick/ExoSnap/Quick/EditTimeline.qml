pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

FocusScope {
    id: root
    required property EditSessionAdapter session
    required property EditPlayerAdapter player
    property EditTimelineAdapter thumbnails: null
    property real pixelsPerSecond: 40
    property bool snapping: true
    property var visibleClips: []
    property real draggedGroup: 0
    property real dragDeltaMs: 0
    readonly property real labelWidth: 50
    readonly property real rulerHeight: 32
    readonly property real rowHeight: 64
    function timeAt(contentPosition: real): real {
        return Math.max(0, (contentPosition - root.labelWidth) / root.pixelsPerMs);
    }
    function positionAt(milliseconds: real): real {
        return root.labelWidth + milliseconds * root.pixelsPerMs;
    }
    function zoomTo(value: real): void {
        const playheadX = root.positionAt(root.session.positionMs) - scroll.contentX;
        const anchorX = playheadX >= root.labelWidth && playheadX <= scroll.width ? playheadX : root.labelWidth;
        const anchorTime = root.timeAt(scroll.contentX + anchorX);
        root.pixelsPerSecond = Math.max(10, Math.min(250, value));
        scroll.contentX = Math.max(0, Math.min(scroll.contentWidth - scroll.width, root.positionAt(anchorTime) - anchorX));
    }
    function refreshClips(): void {
        root.visibleClips = root.session.visibleClips(root.timeAt(scroll.contentX), root.timeAt(scroll.contentX + scroll.width));
    }
    onPixelsPerSecondChanged: refreshClips()
    Component.onCompleted: refreshClips()
    readonly property real pixelsPerMs: pixelsPerSecond / 1000
    objectName: "editClipTimeline"
    activeFocusOnTab: true
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Edit timeline. Left and Right seek; Up and Down select clips. Alt+Left/Right moves; add Control to trim the start or Shift to trim the end. Control+B splits; Delete removes; Shift+Delete closes the gap. Control+wheel or Control+Plus/Minus zooms. Menu opens clip actions.")
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
        if (event.key === Qt.Key_Menu || (shift && event.key === Qt.Key_F10)) {
            if (root.session.selectedClip !== 0) clipMenu.popup();
        }
        else if (control && (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal)) root.zoomTo(root.pixelsPerSecond * 1.25);
        else if (control && event.key === Qt.Key_Minus) root.zoomTo(root.pixelsPerSecond / 1.25);
        else if (control && event.key === Qt.Key_B) root.session.splitSelected();
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
    Menu {
        id: clipMenu
        objectName: "editClipMenu"
        MenuItem {
            objectName: "editRippleDelete"
            text: qsTr("Ripple delete (Shift+Delete)")
            enabled: root.session.selectedClip !== 0
            onTriggered: root.session.deleteSelected(true)
        }
    }
    ToolTip.visible: activeFocus
    ToolTip.text: qsTr("Ctrl+wheel to zoom. Menu or Shift+F10 for clip actions.")
    ToolTip.delay: 1000
    ToolTip.timeout: 4000
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
        objectName: "editTimelineScroll"
        anchors.fill: parent
        anchors.margins: 2
        onContentXChanged: root.refreshClips()
        onWidthChanged: root.refreshClips()
        contentWidth: Math.max(width, root.positionAt(root.session.durationMs + 10000) + 10)
        contentHeight: Math.max(height, root.rulerHeight + root.session.tracks.length * root.rowHeight)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.horizontal: ScrollBar { objectName: "editHorizontalScrollBar"; policy: ScrollBar.AsNeeded }
        ScrollBar.vertical: ScrollBar { objectName: "editVerticalScrollBar"; policy: ScrollBar.AsNeeded }
        WheelHandler {
            acceptedModifiers: Qt.ControlModifier
            onWheel: event => {
                root.zoomTo(root.pixelsPerSecond * Math.pow(1.25, event.angleDelta.y / 120));
                event.accepted = true;
            }
        }
        MouseArea {
            preventStealing: true
            width: scroll.contentWidth
            height: scroll.contentHeight
            onPressed: mouse => {
                root.forceActiveFocus();
                root.player.beginScrub();
                root.session.requestSeek(root.timeAt(mouse.x));
            }
            onPositionChanged: mouse => {
                if (pressed) root.session.requestSeek(root.timeAt(mouse.x));
            }
            onReleased: root.player.endScrub()
            onCanceled: root.player.endScrub()
        }
        Repeater {
            model: root.session.tracks
            delegate: Rectangle {
                required property var modelData
                required property int index
                objectName: "editTrackLabel" + index
                x: scroll.contentX
                y: root.rulerHeight + index * root.rowHeight
                width: root.labelWidth
                height: root.rowHeight
                z: 3
                color: ExoTheme.surface
                Label {
                    anchors.centerIn: parent
                    text: parent.modelData.name
                    color: ExoTheme.textSecondary
                    font.pixelSize: ExoTheme.fontCaption
                }
            }
        }
        Repeater {
            model: root.visibleClips
            delegate: Rectangle {
                id: clipItem
                required property var modelData
                readonly property real visibleLeft: Math.max(10, scroll.contentX + root.labelWidth - x + 6)
                x: root.positionAt(modelData.startMs + (root.draggedGroup === modelData.group ? root.dragDeltaMs : 0))
                y: root.rulerHeight + modelData.trackIndex * root.rowHeight + 3
                width: Math.max(4, modelData.durationMs * root.pixelsPerMs)
                height: 56
                radius: ExoTheme.radiusSm
                color: modelData.selected ? ExoTheme.surfaceHover : ExoTheme.surfaceRaised
                border.color: modelData.selected ? ExoTheme.accent : ExoTheme.line
                border.width: modelData.selected ? 2 : 1
                clip: true
                Accessible.role: Accessible.Button
                Accessible.name: modelData.name + (modelData.available ? "" : ". " + qsTr("Media unavailable"))
                Accessible.onPressAction: root.session.selectClip(modelData.id)
                Image {
                    id: poster
                    x: clipItem.visibleLeft
                    y: 6
                    width: 64
                    height: 44
                    source: clipItem.modelData.video && root.thumbnails && clipItem.modelData.path === root.thumbnails.sourcePath
                            ? root.thumbnails.posterSource : ""
                    visible: status === Image.Ready && clipItem.modelData.available
                    fillMode: Image.PreserveAspectFit
                    Accessible.ignored: true
                }
                ExoGlyph {
                    id: mediaIcon
                    objectName: "editClipMediaIcon"
                    x: clipItem.visibleLeft
                    y: 19
                    width: 16
                    height: 16
                    kind: !clipItem.modelData.available ? ExoGlyph.Warning
                          : clipItem.modelData.video ? ExoGlyph.AppWindow : ExoGlyph.Speaker
                    color: clipItem.modelData.available ? ExoTheme.textMuted : ExoTheme.warning
                    visible: !poster.visible
                    Accessible.ignored: true
                }
                Label {
                    x: clipItem.visibleLeft + (poster.visible ? 70 : 22)
                    y: 19
                    width: Math.max(0, clipItem.width - x - 10)
                    text: clipItem.modelData.name
                    color: clipItem.modelData.available ? ExoTheme.text : ExoTheme.textSecondary
                    font.pixelSize: ExoTheme.fontCaption
                    elide: Text.ElideRight
                }
                MouseArea {
                    id: clipMouse
                    preventStealing: true
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    property real startX: 0
                    onPressed: mouse => {
                        if (mouse.button === Qt.RightButton) {
                            root.session.selectClip(clipItem.modelData.id);
                            root.forceActiveFocus();
                            clipMenu.popup();
                            return;
                        }
                        startX = mapToItem(scroll.contentItem, mouse.x, 0).x;
                        root.draggedGroup = clipItem.modelData.group;
                        root.forceActiveFocus();
                    }
                    onPositionChanged: mouse => {
                        if (pressed && (pressedButtons & Qt.LeftButton)) root.dragDeltaMs = (mapToItem(scroll.contentItem, mouse.x, 0).x - startX) / root.pixelsPerMs;
                    }
                    onCanceled: { root.dragDeltaMs = 0; root.draggedGroup = 0; }
                    onReleased: mouse => {
                        if (mouse.button === Qt.RightButton) return;
                        const delta = root.dragDeltaMs;
                        root.dragDeltaMs = 0;
                        root.draggedGroup = 0;
                        if (Math.abs(delta) > 20) root.session.moveClip(clipItem.modelData.id, clipItem.modelData.startMs + delta, root.snapping);
                        else root.session.selectClip(clipItem.modelData.id);
                    }
                }
                ToolTip.visible: clipMouse.containsMouse
                ToolTip.text: clipItem.Accessible.name + "\n" + clipItem.modelData.path
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
                        Rectangle {
                            anchors.centerIn: parent
                            width: 2
                            height: 20
                            radius: 1
                            color: clipItem.modelData.selected ? ExoTheme.accent : ExoTheme.textMuted
                            visible: clipItem.modelData.selected || clipMouse.containsMouse
                        }
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
        EditTimelineRuler {
            objectName: "editTimelineRuler"
            x: root.labelWidth
            y: scroll.contentY
            z: 4
            width: scroll.contentWidth - root.labelWidth
            height: root.rulerHeight
            pixelsPerMs: root.pixelsPerMs
            viewportX: Math.max(0, scroll.contentX - root.labelWidth)
            viewportWidth: scroll.width
        }
        MouseArea {
            x: root.labelWidth
            preventStealing: true
            y: scroll.contentY
            z: 4
            width: scroll.contentWidth - root.labelWidth
            height: root.rulerHeight
            onPressed: mouse => {
                root.forceActiveFocus();
                root.player.beginScrub();
                root.session.requestSeek(root.timeAt(mouse.x + root.labelWidth));
            }
            onPositionChanged: mouse => {
                if (pressed) root.session.requestSeek(root.timeAt(mouse.x + root.labelWidth));
            }
            onReleased: root.player.endScrub()
            onCanceled: root.player.endScrub()
        }
        Rectangle {
            x: scroll.contentX
            y: scroll.contentY
            width: root.labelWidth
            height: root.rulerHeight
            z: 6
            color: ExoTheme.surface
        }
        Rectangle {
            objectName: "editPlayhead"
            x: root.positionAt(root.session.positionMs)
            visible: x >= scroll.contentX + root.labelWidth
            y: scroll.contentY
            z: 5
            width: 1
            height: scroll.height
            color: ExoTheme.accent
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 5
                height: 4
                radius: 1
                color: ExoTheme.accent
            }
        }
        DropArea {
            width: scroll.contentWidth
            height: scroll.contentHeight
            onDropped: drop => {
                const at = root.timeAt(drop.x);
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
