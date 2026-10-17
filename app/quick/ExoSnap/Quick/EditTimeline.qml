pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

FocusScope {
    id: root
    required property EditSessionAdapter session
    required property EditPlayerAdapter player
    property EditTimelineAdapter thumbnails: null
    property real pixelsPerSecond: 40
    readonly property bool snapping: session.snapping
    property real hoveredGroup: 0
    property bool draggingCrossfade: false
    property bool insertDrag: false
    property var trackToDelete: ({id: 0, name: ""})
    readonly property real labelWidth: 50
    readonly property real rulerHeight: 32
    readonly property real rowHeight: 64
    readonly property real minimumZoom: Math.max(1, scroll.width - labelWidth - 10) * 1000 / Math.max(1, session.durationMs)
    readonly property real maximumZoom: 120000
    function timeAt(contentPosition: real): real {
        return Math.max(0, (contentPosition - root.labelWidth) / root.pixelsPerMs);
    }
    function positionAt(milliseconds: real): real {
        return root.labelWidth + milliseconds * root.pixelsPerMs;
    }
    function zoomTo(value: real): void {
        root.zoomAt(value, -1);
    }
    function zoomAt(value: real, pointerX: real): void {
        const playheadX = root.positionAt(root.session.positionMs) - scroll.contentX;
        const anchorX = pointerX >= root.labelWidth ? pointerX : playheadX >= root.labelWidth && playheadX <= scroll.width ? playheadX : root.labelWidth;
        const anchorTime = root.timeAt(scroll.contentX + anchorX);
        root.pixelsPerSecond = Math.max(root.minimumZoom, Math.min(root.maximumZoom, value));
        scroll.contentX = Math.max(0, Math.min(scroll.contentWidth - scroll.width, root.positionAt(anchorTime) - anchorX));
    }
    function fitTimeline(): void {
        root.pixelsPerSecond = root.minimumZoom;
        scroll.contentX = 0;
    }
    function previewDrag(delta: real, contentY: real, modifiers: int): void {
        root.insertDrag = (modifiers & Qt.ControlModifier) !== 0;
        let track = 0;
        let newTrack = -1;
        const row = Math.floor((contentY - root.rulerHeight) / root.rowHeight);
        const firstAudio = root.session.tracks.findIndex(value => !value.video);
        if (contentY < root.rulerHeight)
            newTrack = 0;
        else if (row >= root.session.tracks.length)
            newTrack = 2;
        else if (firstAudio >= 0 && Math.abs(contentY - root.rulerHeight - firstAudio * root.rowHeight) <= 8)
            newTrack = 1;
        else if (row >= 0)
            track = root.session.tracks[row].id;
        root.session.updateClipDrag(delta, track, newTrack, root.insertDrag, 8 / root.pixelsPerMs);
    }
    function refreshClips(): void {
        clipModel.sync(root.session, root.timeAt(scroll.contentX), root.timeAt(scroll.contentX + scroll.width));
    }
    function visibleCandidates(rows: var): var {
        const from = root.timeAt(scroll.contentX + root.labelWidth);
        const to = root.timeAt(scroll.contentX + scroll.width);
        return rows.filter(value => value.startMs <= to && value.startMs + value.durationMs >= from);
    }
    // Not onPixelsPerSecondChanged: that handler can run before pixelsPerMs is re-evaluated, and
    // the visible window would then be computed at the previous scale.
    onPixelsPerMsChanged: refreshClips()
    Component.onCompleted: refreshClips()
    EditTimelineClipModel {
        id: clipModel
    }
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
        if (event.key === Qt.Key_Escape && root.session.dragActive) {
            root.session.cancelClipDrag();
            event.accepted = true;
            return;
        }
        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && root.session.dragActive) {
            root.session.commitClipDrag();
            event.accepted = true;
            return;
        }
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
            if (root.session.selectedClip !== 0)
                clipMenu.popup();
        } else if (control && (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal))
            root.zoomTo(root.pixelsPerSecond * 1.25);
        else if (control && event.key === Qt.Key_Minus)
            root.zoomTo(root.pixelsPerSecond / 1.25);
        else if (control && event.key === Qt.Key_B)
            root.session.splitSelected();
        else if (control && event.key === Qt.Key_C)
            root.session.copyBlankDuration();
        else if (control && event.key === Qt.Key_V)
            root.session.pasteSpacer();
        else if (!control && !alt && (event.key === Qt.Key_I || event.key === Qt.Key_O))
            root.session.setSelectionBoundary(event.key === Qt.Key_O);
        else if (control && event.key === Qt.Key_Z) {
            if (shift)
                root.session.redo();
            else
                root.session.undo();
        } else if (control && event.key === Qt.Key_Y)
            root.session.redo();
        else if (event.key === Qt.Key_Home)
            root.session.requestSeek(0);
        else if (event.key === Qt.Key_End)
            root.session.requestSeek(root.session.durationMs);
        else if (event.key === Qt.Key_Delete) {
            if (root.session.selectedTransition)
                root.session.removeTransition();
            else
                root.session.deleteSelected(shift);
        } else if (event.key === Qt.Key_Space)
            root.player.togglePlay();
        else if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
            if (shift)
                root.session.requestSeek(root.session.positionMs + direction * 1000);
            else
                root.session.requestFrameStep(direction);
        }
        else
            return;
        event.accepted = true;
    }
    Menu {
        id: gapMenu
        objectName: "editGapMenu"
        MenuItem { text: qsTr("Copy blank duration"); onTriggered: root.session.copyBlankDuration() }
        MenuItem { text: qsTr("Paste Spacer"); onTriggered: root.session.pasteSpacer() }
        MenuItem { text: qsTr("Close Gap (preview)"); onTriggered: root.session.previewCloseGap() }
    }
    Dialog {
        id: trackDeleteConfirmation
        objectName: "editTrackDeleteConfirmation"
        anchors.centerIn: parent
        title: qsTr("Delete track %1?").arg(root.trackToDelete.name)
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label { text: qsTr("Remove this track and all of its timeline content? Source files are kept."); wrapMode: Text.Wrap; width: 280 }
        onAccepted: root.session.deleteTrack(root.trackToDelete.id, true)
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
        MenuItem {
            text: qsTr("Add Crossfade after clip")
            enabled: root.session.selectedClip !== 0 && root.session.canCrossfade()
            onTriggered: root.session.applyCrossfade()
        }
    }
    Popup {
        id: transitionDuration
        objectName: "editCrossfadeDurationPopup"
        anchors.centerIn: parent
        modal: true
        focus: true
        padding: 12
        Column {
            spacing: 8
            Label {
                text: qsTr("Junction window (ms)")
                color: ExoTheme.text
            }
            SpinBox {
                id: durationInput
                objectName: "editCrossfadeDuration"
                from: 1
                to: 600000
                editable: true
                Accessible.name: qsTr("Crossfade duration in milliseconds")
                onValueModified: root.session.setJunctionWindow(windowStart.value, windowStart.value + value)
            }
            SpinBox { id: windowStart; objectName: "editJunctionStart"; from: 0; to: 2147483647; editable: true; Accessible.name: qsTr("Junction start in milliseconds"); onValueModified: root.session.setJunctionWindow(value, value + durationInput.value) }
            ComboBox { id: curveInput; objectName: "editJunctionCurve"; model: [qsTr("Linear"), qsTr("Smooth"), qsTr("Bias")]; Accessible.name: qsTr("Junction curve"); onActivated: root.session.setJunctionCurve(currentIndex, biasInput.value / 100) }
            SpinBox { id: biasInput; objectName: "editJunctionBias"; from: -100; to: 100; editable: true; Accessible.name: qsTr("Curve bias"); onValueModified: root.session.setJunctionCurve(curveInput.currentIndex, value / 100) }
            ComboBox { id: lawInput; objectName: "editJunctionAudioLaw"; model: [qsTr("Equal Gain"), qsTr("Equal Power")]; Accessible.name: qsTr("Audio gain law"); onActivated: root.session.setJunctionAudioLaw(currentIndex) }
            SpinBox { id: cutInput; objectName: "editJunctionCut"; from: 0; to: 2147483647; editable: true; Accessible.name: qsTr("Cut position in milliseconds"); onValueModified: root.session.setJunctionCut(value) }
            Button {
                text: qsTr("Remove Crossfade")
                onClicked: {
                    root.session.removeTransition();
                    transitionDuration.close();
                }
            }
        }
        onOpened: durationInput.forceActiveFocus(Qt.PopupFocusReason)
    }
    ToolTip.visible: activeFocus
    ToolTip.text: qsTr("Ctrl+wheel to zoom. Ctrl+drag previews insertion. I/O marks a time range. Menu or Shift+F10 opens clip actions.")
    ToolTip.delay: 1000
    ToolTip.timeout: 4000
    Connections {
        target: root.session
        function onWorkspaceChanged(): void {
            root.refreshClips();
        }
        function onInteractionSettingsChanged(): void { root.refreshClips(); }
    }
    Button {
        objectName: "editApplyGapPreview"
        anchors.right: parent.right
        anchors.top: parent.top
        z: 20
        visible: root.session.dragActive && root.session.selectedGap.endMs !== undefined
        enabled: root.session.dragValid
        text: root.session.dragValid ? qsTr("Apply gap closure (Enter)") : qsTr("Gap closure unavailable")
        onClicked: root.session.commitClipDrag()
        ToolTip.text: qsTr("Review all outlined clips. Enter applies; Escape cancels.")
        ToolTip.visible: hovered
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
        contentHeight: Math.max(height, root.rulerHeight + Math.max(root.session.tracks.length, root.session.dragTracks.length) * root.rowHeight + (root.session.dragActive ? 16 : 0))
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.horizontal: ScrollBar {
            objectName: "editHorizontalScrollBar"
            policy: ScrollBar.AsNeeded
        }
        ScrollBar.vertical: ScrollBar {
            objectName: "editVerticalScrollBar"
            policy: ScrollBar.AsNeeded
        }
        WheelHandler {
            acceptedModifiers: Qt.ControlModifier
            onWheel: event => {
                const viewportPoint = scroll.contentItem.mapToItem(scroll, event.x, event.y);
                root.zoomAt(root.pixelsPerSecond * Math.pow(1.25, event.angleDelta.y / 120), viewportPoint.x);
                event.accepted = true;
            }
        }
        MouseArea {
            preventStealing: true
            width: scroll.contentWidth
            height: scroll.contentHeight
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onPressed: mouse => {
                root.forceActiveFocus();
                const row = Math.floor((mouse.y - root.rulerHeight) / root.rowHeight);
                if (row >= 0 && row < root.session.tracks.length)
                    root.session.selectGap(root.session.tracks[row].id, root.timeAt(mouse.x));
                if (mouse.button === Qt.RightButton) {
                    if (root.session.selectedGap.endMs !== undefined)
                        gapMenu.popup();
                    return;
                }
                root.player.beginScrub();
                root.session.requestSeek(root.timeAt(mouse.x));
            }
            onPositionChanged: mouse => {
                if (pressed && (pressedButtons & Qt.LeftButton))
                    root.session.requestSeek(root.timeAt(mouse.x));
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
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.RightButton
                    onClicked: {
                        root.trackToDelete = {id: parent.modelData.id, name: parent.modelData.name};
                        trackMenu.popup();
                    }
                    Menu {
                        id: trackMenu
                        MenuItem {
                            text: qsTr("Delete track")
                            onTriggered: if (!root.session.deleteTrack(root.trackToDelete.id)) trackDeleteConfirmation.open()
                        }
                    }
                }
            }
        }
        Rectangle {
            visible: root.session.selectedGap.endMs !== undefined
            x: root.positionAt(root.session.selectedGap.startMs ?? 0)
            y: root.rulerHeight + (root.session.selectedGap.trackIndex ?? 0) * root.rowHeight + 3
            width: Math.max(0, ((root.session.selectedGap.endMs ?? 0) - (root.session.selectedGap.startMs ?? 0)) * root.pixelsPerMs)
            height: 56
            color: "transparent"
            border.color: ExoTheme.accent
            Accessible.ignored: true
        }
        Repeater {
            model: root.session.spacers
            delegate: Rectangle {
                id: spacerItem
                required property var modelData
                objectName: "editSpacer" + modelData.id
                x: root.positionAt(modelData.startMs)
                y: root.rulerHeight + modelData.trackIndex * root.rowHeight + 3
                width: Math.max(4, modelData.durationMs * root.pixelsPerMs)
                height: 56
                color: ExoTheme.surface
                border.color: root.session.selectedSpacer === modelData.id ? ExoTheme.accent : ExoTheme.line
                Label { anchors.centerIn: parent; text: qsTr("Spacer"); color: ExoTheme.textMuted }
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Spacer, %1 ms").arg(modelData.durationMs)
                Accessible.onPressAction: root.session.selectSpacer(modelData.id)
                MouseArea {
                    anchors.fill: parent
                    preventStealing: true
                    property real startX: 0
                    property bool started: false
                    onPressed: mouse => { startX = mapToItem(scroll.contentItem, mouse.x, 0).x; started = false; root.forceActiveFocus(); }
                    onPositionChanged: mouse => {
                        if (!pressed) return;
                        const delta = mapToItem(scroll.contentItem, mouse.x, 0).x - startX;
                        if (!started && Math.abs(delta) >= Qt.styleHints.startDragDistance) { started = true; root.session.beginSpacerDrag(spacerItem.modelData.id); }
                        if (started && root.session.dragActive) root.session.updateClipDrag(delta / root.pixelsPerMs);
                    }
                    onReleased: {
                        if (started) { if (root.session.dragActive) root.session.commitClipDrag(); }
                        else root.session.selectSpacer(spacerItem.modelData.id);
                    }
                    onCanceled: root.session.cancelClipDrag()
                }
                Repeater {
                    model: 2
                    delegate: MouseArea {
                        required property int index
                        property real startX: 0
                        property bool started: false
                        x: index === 0 ? 0 : spacerItem.width - width
                        width: 8; height: spacerItem.height
                        preventStealing: true
                        cursorShape: Qt.SizeHorCursor
                        onPressed: mouse => { startX = mapToItem(scroll.contentItem, mouse.x, 0).x; started = false; root.forceActiveFocus(); }
                        onPositionChanged: mouse => {
                            if (!pressed) return;
                            const delta = mapToItem(scroll.contentItem, mouse.x, 0).x - startX;
                            if (!started && Math.abs(delta) >= Qt.styleHints.startDragDistance) { started = true; root.session.beginSpacerDrag(spacerItem.modelData.id, index); }
                            if (started && root.session.dragActive) root.session.updateClipDrag(delta / root.pixelsPerMs);
                        }
                        onReleased: if (started && root.session.dragActive) root.session.commitClipDrag()
                        onCanceled: root.session.cancelClipDrag()
                    }
                }
            }
        }
        Repeater {
            model: clipModel
            delegate: Rectangle {
                id: clipItem
                required property var modelData
                objectName: "editClip" + modelData.id
                readonly property real visibleLeft: Math.max(10, scroll.contentX + root.labelWidth - x + 6)
                readonly property real visibleWidth: Math.max(0, Math.min(width - visibleLeft, scroll.width - root.labelWidth))
                readonly property real visibleSourceIn: Math.min(modelData.sourceOutUs, modelData.sourceInUs + visibleLeft / root.pixelsPerMs * 1000)
                readonly property real visibleSourceOut: Math.min(modelData.sourceOutUs, visibleSourceIn + visibleWidth / root.pixelsPerMs * 1000)
                readonly property bool cachedVisual: modelData.available && modelData.sourceIdentity.length > 0 && root.thumbnails !== null
                property var thumbnailTiles: []
                property string waveformKey: ""
                property string mediaRequest: ""
                // A delegate outlives most publications, and modelData also changes for selection and
                // timeline position. Only a changed source or visible source window may replace the
                // tile and waveform requests: re-requesting them recreates every tile image.
                function refreshMedia(): void {
                    const shown = cachedVisual && visibleSourceOut > visibleSourceIn;
                    const video = shown && modelData.video;
                    const audio = shown && !modelData.video && modelData.streamIdentity.length > 0;
                    const request = video ? ["video", modelData.sourceIdentity, modelData.path, modelData.width, modelData.height, modelData.fpsNum, modelData.fpsDen, visibleSourceIn, visibleSourceOut, visibleWidth].join("|")
                                          : audio ? ["audio", modelData.sourceIdentity, modelData.path, modelData.containerIndex, modelData.streamIdentity, modelData.sourceEndUs].join("|") : "";
                    if (request === mediaRequest)
                        return;
                    mediaRequest = request;
                    if (video) {
                        root.thumbnails.registerThumbnailSource(modelData.sourceIdentity, modelData.path, modelData.width, modelData.height, modelData.fpsNum, modelData.fpsDen);
                        thumbnailTiles = root.thumbnails.thumbnailTiles(modelData.sourceIdentity, visibleSourceIn, visibleSourceOut, visibleWidth, 44);
                    } else {
                        thumbnailTiles = [];
                    }
                    waveformKey = audio ? root.thumbnails.waveformSource(modelData.sourceIdentity, modelData.path, modelData.containerIndex, modelData.streamIdentity, modelData.sourceEndUs) : "";
                }
                onModelDataChanged: refreshMedia()
                onVisibleSourceInChanged: refreshMedia()
                onVisibleSourceOutChanged: refreshMedia()
                Component.onCompleted: refreshMedia()
                x: root.positionAt(modelData.startMs)
                y: root.rulerHeight + modelData.trackIndex * root.rowHeight + 3
                width: Math.max(4, modelData.durationMs * root.pixelsPerMs)
                height: 56
                radius: ExoTheme.radiusSm
                color: modelData.selected || (root.session.linkedSelection && root.hoveredGroup === modelData.group) ? ExoTheme.surfaceHover : ExoTheme.surfaceRaised
                border.color: modelData.selected ? ExoTheme.accent : ExoTheme.line
                border.width: modelData.selected ? 2 : 1
                clip: true
                Rectangle {
                    anchors.right: parent.right
                    width: 4
                    height: parent.height
                    visible: root.draggingCrossfade && clipItem.modelData.video
                    color: root.session.canCrossfade(clipItem.modelData.id) ? ExoTheme.accent : ExoTheme.warning
                    z: 10
                    Accessible.ignored: true
                }
                Accessible.role: Accessible.Button
                Accessible.name: modelData.name + (modelData.available ? "" : ". " + qsTr("Media unavailable"))
                Accessible.onPressAction: root.session.selectClip(modelData.id)
                Image {
                    id: poster
                    x: clipItem.visibleLeft
                    y: 6
                    width: 64
                    height: 44
                    source: !clipItem.cachedVisual && clipItem.modelData.video && root.thumbnails && clipItem.modelData.path === root.thumbnails.sourcePath ? root.thumbnails.posterSource : ""
                    sourceSize: Qt.size(64, 44)
                    visible: status === Image.Ready && clipItem.modelData.available
                    fillMode: Image.PreserveAspectFit
                    Accessible.ignored: true
                }
                Repeater {
                    model: clipItem.thumbnailTiles
                    delegate: Image {
                        required property var modelData
                        x: clipItem.visibleLeft + modelData.x
                        y: 6
                        width: modelData.width
                        height: 44
                        source: modelData.source
                        sourceSize: Qt.size(width, height)
                        asynchronous: true
                        cache: false
                        visible: status === Image.Ready
                        fillMode: Image.PreserveAspectCrop
                        Accessible.ignored: true
                    }
                }
                ExoWaveformItem {
                    x: clipItem.visibleLeft
                    y: 6
                    width: clipItem.visibleWidth
                    height: 44
                    visible: clipItem.waveformKey.length > 0
                    service: root.thumbnails ? root.thumbnails.waveformService : null
                    cacheKey: clipItem.waveformKey
                    sourceInUs: clipItem.visibleSourceIn
                    sourceOutUs: clipItem.visibleSourceOut
                    channelCount: 2
                    color: ExoTheme.textMuted
                    Accessible.ignored: true
                }
                ExoGlyph {
                    id: mediaIcon
                    objectName: "editClipMediaIcon"
                    x: clipItem.visibleLeft
                    y: 19
                    width: 16
                    height: 16
                    kind: !clipItem.modelData.available ? ExoGlyph.Warning : clipItem.modelData.video ? ExoGlyph.AppWindow : ExoGlyph.Speaker
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
                    onEntered: root.hoveredGroup = clipItem.modelData.group
                    onExited: {
                        if (root.hoveredGroup === clipItem.modelData.group)
                            root.hoveredGroup = 0;
                    }
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    property real startX: 0
                    property real startY: 0
                    property bool started: false
                    onPressed: mouse => {
                        if (mouse.button === Qt.RightButton) {
                            root.session.selectClip(clipItem.modelData.id);
                            root.forceActiveFocus();
                            clipMenu.popup();
                            return;
                        }
                        startX = mapToItem(scroll.contentItem, mouse.x, 0).x;
                        startY = mapToItem(scroll.contentItem, 0, mouse.y).y;
                        started = false;
                        root.forceActiveFocus();
                    }
                    onPositionChanged: mouse => {
                        if (pressed && (pressedButtons & Qt.LeftButton)) {
                            const point = mapToItem(scroll.contentItem, mouse.x, mouse.y);
                            if (!started && Math.hypot(point.x - startX, point.y - startY) >= Qt.styleHints.startDragDistance) {
                                started = true;
                                root.session.beginClipDrag(clipItem.modelData.id);
                            }
                            if (started && root.session.dragActive)
                                root.previewDrag((point.x - startX) / root.pixelsPerMs, point.y, mouse.modifiers);
                        }
                    }
                    onCanceled: {
                        root.session.cancelClipDrag();
                    }
                    onReleased: mouse => {
                        if (mouse.button === Qt.RightButton)
                            return;
                        if (started) {
                            if (root.session.dragActive)
                                root.session.commitClipDrag();
                        } else
                            root.session.selectClip(clipItem.modelData.id);
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
                        property bool started: false
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
                        onPressed: mouse => {
                            startX = mapToItem(scroll.contentItem, mouse.x, 0).x;
                            started = false;
                            root.forceActiveFocus();
                        }
                        onPositionChanged: mouse => {
                            if (!pressed)
                                return;
                            const delta = mapToItem(scroll.contentItem, mouse.x, 0).x - startX;
                            if (!started && Math.abs(delta) >= Qt.styleHints.startDragDistance) {
                                started = true;
                                root.session.beginClipDrag(clipItem.modelData.id, index);
                            }
                            if (started && root.session.dragActive)
                                root.session.updateClipDrag(delta / root.pixelsPerMs, 0, -1, false, 8 / root.pixelsPerMs);
                        }
                        onReleased: if (started && root.session.dragActive) root.session.commitClipDrag()
                        onCanceled: root.session.cancelClipDrag()
                    }
                }
                Repeater {
                    model: clipItem.modelData.selected ? 2 : 0
                    delegate: MouseArea {
                        id: fadeHandle
                        required property int index
                        objectName: "editFadeHandle" + clipItem.modelData.id + "_" + index
                        readonly property real fadeMs: index === 0 ? clipItem.modelData.fadeInMs : clipItem.modelData.fadeOutMs
                        property real startX: 0
                        property bool started: false
                        x: index === 0 ? Math.max(8, fadeMs * root.pixelsPerMs) : clipItem.width - Math.max(16, fadeMs * root.pixelsPerMs + 8)
                        y: 0
                        width: 8
                        height: 12
                        cursorShape: Qt.SizeHorCursor
                        Rectangle { anchors.fill: parent; color: ExoTheme.accent; radius: 2; Accessible.ignored: true }
                        onPressed: mouse => { startX = mapToItem(scroll.contentItem, mouse.x, 0).x; started = false; root.forceActiveFocus(); }
                        onPositionChanged: mouse => {
                            if (!pressed) return;
                            const delta = mapToItem(scroll.contentItem, mouse.x, 0).x - startX;
                            if (!started && Math.abs(delta) >= Qt.styleHints.startDragDistance) { started = true; root.session.beginClipDrag(clipItem.modelData.id, index + 2); }
                            if (started && root.session.dragActive) root.session.updateClipDrag(delta / root.pixelsPerMs);
                        }
                        onReleased: if (started && root.session.dragActive) root.session.commitClipDrag()
                        onCanceled: root.session.cancelClipDrag()
                        Accessible.role: Accessible.Slider
                        Accessible.name: index === 0 ? qsTr("Fade in") : qsTr("Fade out")
                        activeFocusOnTab: true
                    }
                }
            }
        }
        Repeater {
            model: root.session.dragActive ? root.visibleCandidates(root.session.dragClips) : []
            delegate: Rectangle {
                id: clipGhost
                required property var modelData
                objectName: "editDragGhost" + modelData.id
                x: Math.max(scroll.contentX + root.labelWidth, root.positionAt(modelData.startMs))
                y: root.rulerHeight + modelData.trackIndex * root.rowHeight + 3
                width: Math.max(0, Math.min(scroll.contentX + scroll.width, root.positionAt(modelData.startMs + modelData.durationMs)) - x)
                height: 56
                z: 7
                color: "transparent"
                border.width: 2
                border.color: root.session.dragValid ? ExoTheme.accent : ExoTheme.warning
                radius: ExoTheme.radiusSm
                Accessible.ignored: true
                Label {
                    anchors.centerIn: parent
                    text: root.session.dragValid ? root.session.formatTimestamp(parent.modelData.startMs) : qsTr("Unavailable")
                    color: root.session.dragValid ? ExoTheme.accent : ExoTheme.warning
                }
                Rectangle { x: root.positionAt(clipGhost.modelData.startMs + clipGhost.modelData.fadeInMs) - clipGhost.x; visible: x >= 0 && x <= clipGhost.width - width; y: 0; width: 2; height: 12; color: ExoTheme.accent; Accessible.ignored: true }
                Rectangle { x: root.positionAt(clipGhost.modelData.startMs + clipGhost.modelData.durationMs - clipGhost.modelData.fadeOutMs) - clipGhost.x - 2; visible: x >= 0 && x <= clipGhost.width - width; y: 0; width: 2; height: 12; color: ExoTheme.accent; Accessible.ignored: true }
            }
        }
        Repeater {
            model: root.session.dragActive ? root.visibleCandidates(root.session.dragSpacers) : []
            delegate: Rectangle {
                required property var modelData
                objectName: "editSpacerGhost" + modelData.id
                x: Math.max(scroll.contentX + root.labelWidth, root.positionAt(modelData.startMs))
                y: root.rulerHeight + modelData.trackIndex * root.rowHeight + 3
                width: Math.max(0, Math.min(scroll.contentX + scroll.width, root.positionAt(modelData.startMs + modelData.durationMs)) - x)
                height: 56
                z: 7
                color: "transparent"
                border.width: 2
                border.color: root.session.dragValid ? ExoTheme.accent : ExoTheme.warning
                Accessible.ignored: true
            }
        }
        Repeater {
            model: root.session.dragActive ? root.visibleCandidates(root.session.dragTransitions) : []
            delegate: Rectangle {
                required property var modelData
                objectName: "editDragJunctionGhost"
                x: Math.max(scroll.contentX + root.labelWidth, root.positionAt(modelData.startMs))
                y: root.rulerHeight + modelData.trackIndex * root.rowHeight + 20
                width: Math.max(0, Math.min(scroll.contentX + scroll.width, root.positionAt(modelData.startMs + modelData.durationMs)) - x)
                height: 24
                z: 8
                color: "transparent"
                border.color: root.session.dragValid ? ExoTheme.accent : ExoTheme.warning
                Accessible.ignored: true
            }
        }
        Repeater {
            model: root.session.dragActive ? [0, 1, 2] : []
            delegate: Rectangle {
                required property int modelData
                readonly property int audioIndex: root.session.tracks.findIndex(value => !value.video)
                objectName: "editNewTrackDrop" + modelData
                x: scroll.contentX + root.labelWidth
                y: modelData === 0 ? root.rulerHeight - 8 : modelData === 1 ? root.rulerHeight + (audioIndex < 0 ? root.session.tracks.length : audioIndex) * root.rowHeight - 4 : root.rulerHeight + root.session.tracks.length * root.rowHeight
                width: scroll.width - root.labelWidth
                height: 8
                z: 8
                color: ExoTheme.accent
                Accessible.ignored: true
                Label { anchors.left: parent.left; anchors.bottom: parent.top; text: parent.modelData === 0 ? qsTr("New front video layer") : parent.modelData === 1 ? qsTr("New rear video layer") : qsTr("New audio lane"); color: ExoTheme.accent; Accessible.ignored: true }
            }
        }
        Rectangle {
            visible: root.session.dragActive && root.insertDrag && root.session.dragClips.length > 0
            x: root.positionAt(root.session.dragClips[0]?.startMs ?? 0)
            y: root.rulerHeight
            width: 2
            height: scroll.contentHeight - root.rulerHeight
            z: 8
            color: root.session.dragValid ? ExoTheme.accent : ExoTheme.warning
            Accessible.ignored: true
        }
        Repeater {
            model: root.session.transitions
            delegate: Rectangle {
                id: transitionOverlay
                required property var modelData
                objectName: "editCrossfadeOverlay"
                x: root.positionAt(modelData.startMs)
                y: root.rulerHeight + modelData.trackIndex * root.rowHeight + 20
                width: Math.max(14, modelData.durationMs * root.pixelsPerMs)
                height: 24
                z: 3
                radius: 3
                color: modelData.selected ? ExoTheme.accent : ExoTheme.surface
                border.color: ExoTheme.accent
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Crossfade, %1 ms. Enter to adjust duration.").arg(modelData.durationMs)
                function adjust(): void {
                    root.session.selectTransition(modelData.outgoing);
                    durationInput.value = modelData.durationMs;
                    windowStart.value = modelData.startMs;
                    curveInput.currentIndex = modelData.curve;
                    biasInput.value = modelData.bias * 100;
                    lawInput.currentIndex = modelData.audioLaw;
                    lawInput.visible = !modelData.video;
                    cutInput.value = modelData.cutMs;
                    transitionDuration.open();
                }
                Keys.onReturnPressed: adjust()
                Keys.onSpacePressed: adjust()
                Keys.onDeletePressed: {
                    root.session.selectTransition(modelData.outgoing);
                    root.session.removeTransition();
                }
                Label {
                    anchors.centerIn: parent
                    text: "╲══╱"
                    color: ExoTheme.text
                    font.pixelSize: ExoTheme.fontCaption
                    Accessible.ignored: true
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        root.session.selectTransition(transitionOverlay.modelData.outgoing);
                        transitionOverlay.forceActiveFocus();
                    }
                    onDoubleClicked: transitionOverlay.adjust()
                }
                Repeater {
                    model: 2
                    delegate: MouseArea {
                        required property int index
                        property real startX: 0
                        property bool started: false
                        objectName: "editJunctionHandle" + transitionOverlay.modelData.id + "_" + index
                        x: index === 0 ? 0 : transitionOverlay.width - width
                        width: 6
                        height: transitionOverlay.height
                        cursorShape: Qt.SizeHorCursor
                        onPressed: mouse => { startX = mapToItem(scroll.contentItem, mouse.x, 0).x; started = false; root.forceActiveFocus(); }
                        onPositionChanged: mouse => {
                            if (!pressed)
                                return;
                            const delta = mapToItem(scroll.contentItem, mouse.x, 0).x - startX;
                            if (!started && Math.abs(delta) >= Qt.styleHints.startDragDistance) {
                                started = true;
                                root.session.beginJunctionDrag(transitionOverlay.modelData.outgoing, index);
                            }
                            if (started && root.session.dragActive)
                                root.session.updateClipDrag(delta / root.pixelsPerMs);
                        }
                        onReleased: if (started && root.session.dragActive) root.session.commitClipDrag()
                        onCanceled: root.session.cancelClipDrag()
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
                if (pressed)
                    root.session.requestSeek(root.timeAt(mouse.x + root.labelWidth));
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
            onEntered: drag => {
                root.draggingCrossfade = drag.formats.indexOf("application/x-exosnap-crossfade") >= 0;
            }
            onExited: root.draggingCrossfade = false
            onDropped: drop => {
                root.draggingCrossfade = false;
                const at = root.timeAt(drop.x);
                if (drop.formats.indexOf("application/x-exosnap-crossfade") >= 0) {
                    if (root.session.dropCrossfade(at, 10 / root.pixelsPerMs))
                        drop.acceptProposedAction();
                    return;
                }
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
