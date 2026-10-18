pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

FocusScope {
    id: root
    required property EditSessionAdapter session
    required property EditPlayerAdapter player
    property EditTimelineAdapter thumbnails: null
    property Item keyboardScope: root
    property alias pixelsPerSecond: viewport.pixelsPerSecond
    readonly property bool snapping: session.snapping
    property real hoveredGroup: 0
    property bool draggingCrossfade: false
    property bool insertDrag: false
    property var trackToDelete: ({id: 0, name: ""})
    readonly property real labelWidth: 50
    readonly property real rulerHeight: 32
    readonly property EditTrackLayoutModel trackLayout: session.trackLayout
    readonly property real minimumZoom: viewport.minimumZoom
    readonly property real maximumZoom: viewport.maximumZoom
    function timeAtUs(contentPosition: real): real {
        return viewport.mapping.timeAtX(contentPosition - root.labelWidth);
    }
    function timeAt(contentPosition: real): real {
        return root.timeAtUs(contentPosition) / 1000;
    }
    function positionAtUs(time: real): real {
        return root.labelWidth + viewport.mapping.xAtTime(time);
    }
    function positionAt(milliseconds: real): real {
        return root.positionAtUs(Math.round(milliseconds * 1000));
    }
    function visibleX(timeUs: real): real {
        return Math.max(root.labelWidth, Math.min(scroll.width, root.positionAtUs(timeUs)));
    }
    function visibleSpan(fromUs: real, toUs: real): real {
        return Math.max(0, root.visibleX(toUs) - root.visibleX(fromUs));
    }
    function trackGeometry(trackId: var, preview: bool): var {
        return (preview ? root.trackLayout.previewGeometry : root.trackLayout.geometry)[String(trackId)] ?? {top: 0, height: 0};
    }
    function zoomTo(value: real): void {
        root.zoomAt(value, -1);
    }
    function zoomAt(value: real, pointerX: real): void {
        const playheadX = root.positionAtUs(root.session.positionUs);
        const anchorX = pointerX >= root.labelWidth ? pointerX : playheadX >= root.labelWidth && playheadX <= scroll.width ? playheadX : root.labelWidth;
        viewport.zoomAt(Math.max(root.minimumZoom, Math.min(root.maximumZoom, value)), anchorX - root.labelWidth);
    }
    function fitTimeline(): void {
        viewport.fitTimeline();
    }
    function panTimeline(pixels: real): void {
        viewport.panPixels(pixels);
    }
    function previewDrag(delta: real, contentY: real, modifiers: int): void {
        root.insertDrag = (modifiers & Qt.ControlModifier) !== 0;
        const destination = root.trackLayout.dropTargetAtY(contentY - root.rulerHeight);
        root.session.updateClipDragUs(Math.round(delta), destination.trackId ?? 0, destination.newTrack ?? -1, root.insertDrag, Math.round(8 / root.pixelsPerUs));
    }
    property bool visualWorkPending: false
    property bool clipSyncPending: false
    function scheduleVisualWork(): void {
        visualWorkPending = true;
        if (root.visible && root.Window.window)
            root.Window.window.update();
    }
    function scheduleClipSync(): void {
        clipSyncPending = true;
        scheduleVisualWork();
    }
    function refreshClips(): void {
        clipSyncPending = false;
        clipModel.syncUs(root.session, viewport.startUs, viewport.endUs);
    }
    function flushVisualWork(): void {
        visualWorkPending = false;
        if (clipSyncPending)
            refreshClips();
        for (let i = 0; i < clipRepeater.count; ++i) {
            const item = clipRepeater.itemAt(i);
            if (item)
                item.refreshMedia();
        }
    }
    onVisibleChanged: {
        if (root.visible && visualWorkPending)
            scheduleVisualWork();
    }
    Connections {
        target: root.visible && root.visualWorkPending ? root.Window.window : null
        function onAfterAnimating(): void { root.flushVisualWork(); }
    }
    Connections {
        target: root.thumbnails
        function onMediaCachesCleared(): void {
            for (let i = 0; i < clipRepeater.count; ++i) {
                const item = clipRepeater.itemAt(i);
                if (item)
                    item.retireMedia();
            }
            root.scheduleVisualWork();
        }
    }
    function visibleCandidates(rows: var): var {
        return rows.filter(value => value.startUs <= viewport.endUs && value.endUs >= viewport.startUs);
    }
    Component.onCompleted: refreshClips()
    EditViewportAdapter {
        id: viewport
        objectName: "editViewport"
        timelineEndUs: root.session.durationUs
        usableWidth: Math.max(1, scroll.width - root.labelWidth)
        onViewChanged: root.scheduleClipSync()
    }
    EditInteractionController {
        id: interaction
        objectName: "editInteraction"
        scopeItem: root.keyboardScope
        blocked: root.session.dragActive || !root.visible || !root.enabled
        onTogglePlayRequested: root.player.togglePlay()
        onPanRequested: pixels => root.panTimeline(pixels)
    }
    EditTimelineClipModel {
        id: clipModel
        objectName: "editVisibleClips"
    }
    readonly property real pixelsPerMs: pixelsPerSecond / 1000
    readonly property real pixelsPerUs: pixelsPerSecond / 1000000
    objectName: "editClipTimeline"
    activeFocusOnTab: true
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Edit timeline. Release Space to play or pause; hold Space and drag to pan. Left and Right seek; Up and Down select clips. Alt+Left/Right moves; add Control to trim the start or Shift to trim the end. Control+B splits; Delete removes; Shift+Delete closes the gap. Control+wheel or Control+Plus/Minus zooms. Menu opens clip actions.")
    Keys.priority: Keys.AfterItem
    Keys.onPressed: event => {
        const focused = root.Window.window ? root.Window.window.activeFocusItem : null;
        if (focused instanceof TextInput || focused instanceof TextEdit)
            return;
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
            root.session.requestSeekUs(root.session.durationUs);
        else if (event.key === Qt.Key_Delete) {
            if (root.session.selectedTransition)
                root.session.removeTransition();
            else
                root.session.deleteSelected(shift);
        } else if (event.key === Qt.Key_Space) {
            event.accepted = interaction.armSpace(event.modifiers, event.isAutoRepeat);
            return;
        } else if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
            if (shift)
                root.session.requestSeekUs(root.session.positionUs + direction * 1000000);
            else
                root.session.requestFrameStep(direction);
        }
        else
            return;
        event.accepted = true;
    }
    // Opens the junction popup for one transition row, as Enter on its overlay
    // does.
    function openTransitionPopup(row: var): void {
        root.session.selectTransition(row.outgoing);
        durationInput.value = row.durationMs;
        windowStart.value = row.startMs;
        biasInput.value = row.bias * 100;
        lawRow.visible = !row.video;
        cutInput.value = row.cutMs;
        transitionDuration.open();
    }
    ExoMenu {
        id: gapMenu
        objectName: "editGapMenu"
        ExoMenuItem { text: qsTr("Copy blank duration"); onTriggered: root.session.copyBlankDuration() }
        ExoMenuItem { text: qsTr("Paste Spacer"); onTriggered: root.session.pasteSpacer() }
        ExoMenuItem { text: qsTr("Close Gap (preview)"); onTriggered: root.session.previewCloseGap() }
    }
    ExoConfirmDialog {
        id: trackDeleteConfirmation
        objectName: "editTrackDeleteConfirmation"
        title: qsTr("Delete track %1?").arg(root.trackToDelete.name)
        bodyText: qsTr("Remove this track and all of its timeline content? Source files are kept.")
        proceedText: qsTr("Delete track")
        onAccepted: root.session.deleteTrack(root.trackToDelete.id, true)
    }
    ExoMenu {
        id: clipMenu
        objectName: "editClipMenu"
        ExoMenuItem {
            objectName: "editRippleDelete"
            text: qsTr("Ripple delete (Shift+Delete)")
            enabled: root.session.selectedClip !== 0
            onTriggered: root.session.deleteSelected(true)
        }
        ExoMenuItem {
            text: qsTr("Add Crossfade after clip")
            enabled: root.session.selectedClip !== 0 && root.session.canCrossfade()
            onTriggered: root.session.applyCrossfade()
        }
    }
    Popup {
        id: transitionDuration
        objectName: "editCrossfadeDurationPopup"
        // The window's centre, not the timeline's: the timeline card is often
        // shorter than the popup.
        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        padding: ExoTheme.spacingLg
        width: 300
        // See ExoConfirmDialog: the style's own veil lightens the dark shell.
        Overlay.modal: Rectangle {
            color: ExoTheme.overlayScrim
        }
        background: Rectangle {
            color: ExoTheme.surfaceRaised
            border.width: 1
            border.color: ExoTheme.lineStrong
            radius: ExoTheme.radiusLg
        }
        contentItem: Column {
            spacing: ExoTheme.spacingSm
            Label {
                text: qsTr("Crossfade")
                color: ExoTheme.text
                font.family: ExoTheme.sansFamily
                font.pixelSize: ExoTheme.fontSectionTitle
                font.weight: Font.DemiBold
            }
            ExoSettingRow {
                width: parent.width
                label: qsTr("Duration")
                stacked: true
                ExoNumberField {
                    id: durationInput
                    objectName: "editCrossfadeDuration"
                    Layout.fillWidth: true
                    from: 1
                    to: 600000
                    suffix: qsTr("ms")
                    Accessible.name: qsTr("Crossfade duration in milliseconds")
                    onValueModified: root.session.setJunctionWindow(windowStart.value, windowStart.value + value)
                }
            }
            ExoSettingRow {
                width: parent.width
                label: qsTr("Start")
                stacked: true
                ExoNumberField {
                    id: windowStart
                    objectName: "editJunctionStart"
                    Layout.fillWidth: true
                    from: 0
                    to: 2147483647
                    suffix: qsTr("ms")
                    Accessible.name: qsTr("Junction start in milliseconds")
                    onValueModified: root.session.setJunctionWindow(value, value + durationInput.value)
                }
            }
            ExoSettingRow {
                width: parent.width
                label: qsTr("Curve")
                stacked: true
                ExoSelect {
                    id: curveInput
                    objectName: "editJunctionCurve"
                    Layout.fillWidth: true
                    options: [
                        { value: 0, label: qsTr("Linear") },
                        { value: 1, label: qsTr("Smooth") },
                        { value: 2, label: qsTr("Bias") }
                    ]
                    value: root.session.selectedJunctionDetails.curve ?? 0
                    Accessible.name: qsTr("Junction curve")
                    onValueActivated: value => root.session.setJunctionCurve(value, biasInput.value / 100)
                }
            }
            ExoSettingRow {
                width: parent.width
                label: qsTr("Curve bias")
                stacked: true
                ExoNumberField {
                    id: biasInput
                    objectName: "editJunctionBias"
                    Layout.fillWidth: true
                    from: -100
                    to: 100
                    Accessible.name: qsTr("Curve bias")
                    onValueModified: root.session.setJunctionCurve(curveInput.value, value / 100)
                }
            }
            ExoSettingRow {
                id: lawRow
                width: parent.width
                label: qsTr("Audio gain law")
                stacked: true
                ExoSelect {
                    id: lawInput
                    objectName: "editJunctionAudioLaw"
                    Layout.fillWidth: true
                    options: [
                        { value: 0, label: qsTr("Equal Gain") },
                        { value: 1, label: qsTr("Equal Power") }
                    ]
                    value: root.session.selectedJunctionDetails.audioLaw ?? 0
                    Accessible.name: qsTr("Audio gain law")
                    onValueActivated: value => root.session.setJunctionAudioLaw(value)
                }
            }
            ExoSettingRow {
                width: parent.width
                label: qsTr("Cut position")
                stacked: true
                ExoNumberField {
                    id: cutInput
                    objectName: "editJunctionCut"
                    Layout.fillWidth: true
                    from: 0
                    to: 2147483647
                    suffix: qsTr("ms")
                    Accessible.name: qsTr("Cut position in milliseconds")
                    onValueModified: root.session.setJunctionCut(value)
                }
            }
            ExoButton {
                objectName: "editRemoveCrossfade"
                width: parent.width
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
    ExoButton {
        objectName: "editApplyGapPreview"
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: ExoTheme.spacingSm
        z: 20
        tone: "primary"
        compact: true
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
        border.color: ExoTheme.line
    }
    Flickable {
        id: scroll
        objectName: "editTimelineScroll"
        anchors.fill: parent
        anchors.margins: 2
        contentWidth: width
        flickableDirection: Flickable.VerticalFlick
        contentHeight: Math.max(height, root.rulerHeight + root.trackLayout.totalHeight + (root.session.dragActive ? 16 : 0))
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ExoScrollBar {
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
        WheelHandler {
            acceptedModifiers: Qt.ShiftModifier
            onWheel: event => {
                root.panTimeline(-(event.pixelDelta.x || event.pixelDelta.y || event.angleDelta.x / 3 || event.angleDelta.y / 3));
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
                const track = root.trackLayout.trackAtY(mouse.y - root.rulerHeight);
                if (track)
                    root.session.selectGapUs(track, root.timeAtUs(mouse.x));
                if (mouse.button === Qt.RightButton) {
                    if (root.session.selectedGap.endMs !== undefined)
                        gapMenu.popup();
                    return;
                }
                root.player.beginScrub();
                root.session.requestSeekUs(root.timeAtUs(mouse.x));
            }
            onPositionChanged: mouse => {
                if (pressed && (pressedButtons & Qt.LeftButton))
                    root.session.requestSeekUs(root.timeAtUs(mouse.x));
            }
            onReleased: root.player.endScrub()
            onCanceled: root.player.endScrub()
        }
        Repeater {
            model: root.trackLayout.rows
            delegate: Rectangle {
                required property var modelData
                required property int index
                objectName: "editTrackLabel" + index
                x: scroll.contentX
                y: root.rulerHeight + modelData.top
                width: root.labelWidth
                height: modelData.height
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
                    ExoMenu {
                        id: trackMenu
                        ExoMenuItem {
                            text: qsTr("Delete track")
                            onTriggered: if (!root.session.deleteTrack(root.trackToDelete.id)) trackDeleteConfirmation.open()
                        }
                    }
                }
            }
        }
        Rectangle {
            visible: root.session.selectedGap.endMs !== undefined
            readonly property var trackBounds: root.trackGeometry(root.session.selectedGap.trackId ?? 0, false)
            x: root.visibleX(root.session.selectedGap.startUs ?? 0)
            y: root.rulerHeight + trackBounds.top
            width: root.visibleSpan(root.session.selectedGap.startUs ?? 0, root.session.selectedGap.endUs ?? 0)
            height: trackBounds.height
            color: "transparent"
            border.color: ExoTheme.accent
            Accessible.ignored: true
        }
        Repeater {
            model: root.session.spacers
            delegate: Rectangle {
                id: spacerItem
                readonly property var trackBounds: root.trackGeometry(modelData.trackId, false)
                required property var modelData
                objectName: "editSpacer" + modelData.id
                x: root.visibleX(modelData.startUs)
                y: root.rulerHeight + trackBounds.top
                width: root.visibleSpan(modelData.startUs, modelData.endUs)
                height: trackBounds.height
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
                        if (started && root.session.dragActive) root.session.updateClipDragUs(Math.round(delta / root.pixelsPerUs));
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
                        visible: (index === 0 ? spacerItem.modelData.startUs >= viewport.startUs : spacerItem.modelData.endUs <= viewport.endUs) && spacerItem.width > 0
                        width: 8; height: spacerItem.height
                        preventStealing: true
                        cursorShape: Qt.SizeHorCursor
                        onPressed: mouse => { startX = mapToItem(scroll.contentItem, mouse.x, 0).x; started = false; root.forceActiveFocus(); }
                        onPositionChanged: mouse => {
                            if (!pressed) return;
                            const delta = mapToItem(scroll.contentItem, mouse.x, 0).x - startX;
                            if (!started && Math.abs(delta) >= Qt.styleHints.startDragDistance) { started = true; root.session.beginSpacerDrag(spacerItem.modelData.id, index); }
                            if (started && root.session.dragActive) root.session.updateClipDragUs(Math.round(delta / root.pixelsPerUs));
                        }
                        onReleased: if (started && root.session.dragActive) root.session.commitClipDrag()
                        onCanceled: root.session.cancelClipDrag()
                    }
                }
            }
        }
        Repeater {
            id: clipRepeater
            model: clipModel
            delegate: Rectangle {
                id: clipItem
                readonly property var trackBounds: root.trackGeometry(modelData.trackId, false)
                required property var modelData
                objectName: "editClip" + modelData.id
                readonly property real visibleLeft: Math.min(6, width / 2)
                readonly property real visibleWidth: Math.max(0, width - 2 * visibleLeft)
                readonly property real visibleSourceIn: Math.min(modelData.sourceOutUs, modelData.sourceInUs + Math.max(0, viewport.startUs - modelData.startUs) + visibleLeft / root.pixelsPerUs)
                readonly property real visibleSourceOut: Math.min(modelData.sourceOutUs, visibleSourceIn + visibleWidth / root.pixelsPerUs)
                readonly property bool bodyVisible: trackBounds.height > root.trackLayout.minimumHeight
                readonly property real bodyHeight: Math.max(0, Math.min(44, trackBounds.height - root.trackLayout.headerHeight - 2))
                readonly property bool previewVisible: bodyVisible && root.visible && y + root.trackLayout.headerHeight + 1 + bodyHeight > scroll.contentY + root.rulerHeight && y + root.trackLayout.headerHeight + 1 < scroll.contentY + scroll.height
                readonly property bool cachedVisual: previewVisible && modelData.available && modelData.sourceIdentity.length > 0 && root.thumbnails !== null
                property string waveformKey: ""
                property string mediaRequest: ""
                // A delegate outlives most publications, and modelData also changes for selection and
                // timeline position. Only a changed source or visible source window may replace the
                // tile and waveform requests. Tiles are keyed by source time, so a window that shrinks
                // or grows keeps the tile images it still shows instead of reloading every one.
                EditTimelineClipModel { id: tileModel }
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
                        tileModel.setRows(root.thumbnails.thumbnailTiles(modelData.sourceIdentity, visibleSourceIn, visibleSourceOut, visibleWidth, 44));
                    } else {
                        tileModel.setRows([]);
                    }
                    waveformKey = audio ? root.thumbnails.waveformSource(modelData.sourceIdentity, modelData.path, modelData.containerIndex, modelData.streamIdentity, modelData.sourceEndUs) : "";
                }
                readonly property string mediaSource: [modelData.video, modelData.sourceIdentity, modelData.path, modelData.containerIndex, modelData.streamIdentity].join("|")
                function retireMedia(): void {
                    mediaRequest = "";
                    tileModel.setRows([]);
                    waveformKey = "";
                }
                function requestMediaRefresh(): void {
                    if (!cachedVisual || visibleSourceOut <= visibleSourceIn)
                        retireMedia();
                    else
                        root.scheduleVisualWork();
                }
                onMediaSourceChanged: {
                    retireMedia();
                    requestMediaRefresh();
                }
                onModelDataChanged: requestMediaRefresh()
                onCachedVisualChanged: requestMediaRefresh()
                onVisibleSourceInChanged: requestMediaRefresh()
                onVisibleSourceOutChanged: requestMediaRefresh()
                onVisibleWidthChanged: requestMediaRefresh()
                Component.onCompleted: requestMediaRefresh()
                x: root.visibleX(modelData.startUs)
                y: root.rulerHeight + trackBounds.top
                width: root.visibleSpan(modelData.startUs, modelData.endUs)
                height: trackBounds.height
                color: modelData.selected || (root.session.linkedSelection && root.hoveredGroup === modelData.group) ? ExoTheme.surfaceHover : ExoTheme.surfaceRaised
                border.color: modelData.selected ? ExoTheme.accent : ExoTheme.line
                border.width: 1
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
                    objectName: "editClipPoster" + clipItem.modelData.id
                    x: clipItem.visibleLeft
                    y: root.trackLayout.headerHeight + 1
                    width: 64
                    height: clipItem.bodyHeight
                    source: clipItem.previewVisible && !clipItem.cachedVisual && clipItem.modelData.video && root.thumbnails && clipItem.modelData.path === root.thumbnails.sourcePath ? root.thumbnails.posterSource : ""
                    sourceSize: Qt.size(64, 44)
                    visible: status === Image.Ready && clipItem.modelData.available && clipItem.previewVisible
                    fillMode: Image.PreserveAspectFit
                    Accessible.ignored: true
                }
                Repeater {
                    model: tileModel
                    delegate: Image {
                        objectName: "editClipThumbnail"
                        required property var modelData
                        x: clipItem.visibleLeft + modelData.x
                        y: root.trackLayout.headerHeight + 1
                        width: modelData.width
                        height: clipItem.bodyHeight
                        source: modelData.source
                        sourceSize: Qt.size(width, 44)
                        asynchronous: true
                        cache: false
                        visible: status === Image.Ready && clipItem.previewVisible
                        fillMode: Image.PreserveAspectCrop
                        Accessible.ignored: true
                    }
                }
                ExoWaveformItem {
                    objectName: "editClipWaveform" + clipItem.modelData.id
                    x: clipItem.visibleLeft
                    y: root.trackLayout.headerHeight + 1
                    width: clipItem.visibleWidth
                    height: clipItem.bodyHeight
                    visible: clipItem.waveformKey.length > 0 && clipItem.previewVisible
                    service: root.thumbnails ? root.thumbnails.waveformService : null
                    cacheKey: clipItem.waveformKey
                    sourceInUs: clipItem.visibleSourceIn
                    sourceOutUs: clipItem.visibleSourceOut
                    channelCount: clipItem.modelData.sourceChannels === 1 ? 1 : 2
                    color: ExoTheme.textMuted
                    Accessible.ignored: true
                }
                ExoGlyph {
                    id: mediaIcon
                    objectName: "editClipMediaIcon"
                    x: clipItem.visibleLeft
                    y: 1
                    width: 16
                    height: 16
                    kind: !clipItem.modelData.available ? ExoGlyph.Warning : clipItem.modelData.video ? ExoGlyph.AppWindow : ExoGlyph.Speaker
                    color: clipItem.modelData.available ? ExoTheme.textMuted : ExoTheme.warning
                    Accessible.ignored: true
                }
                Label {
                    x: clipItem.visibleLeft + 22
                    y: 0
                    height: root.trackLayout.headerHeight
                    verticalAlignment: Text.AlignVCenter
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
                                root.previewDrag((point.x - startX) / root.pixelsPerUs, point.y, mouse.modifiers);
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
                        visible: (index === 0 ? clipItem.modelData.startUs >= viewport.startUs : clipItem.modelData.endUs <= viewport.endUs) && clipItem.width > 0
                        width: 8
                        height: clipItem.height
                        cursorShape: Qt.SizeHorCursor

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
                                root.session.updateClipDragUs(Math.round(delta / root.pixelsPerUs), 0, -1, false, Math.round(8 / root.pixelsPerUs));
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
                        readonly property real fadeTimeUs: index === 0 ? clipItem.modelData.startUs + clipItem.modelData.fadeInUs : clipItem.modelData.endUs - clipItem.modelData.fadeOutUs
                        property real startX: 0
                        property bool started: false
                        x: root.positionAtUs(fadeTimeUs) - clipItem.x - (index === 0 ? 0 : width)
                        visible: x >= 0 && x + width <= clipItem.width
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
                            if (started && root.session.dragActive) root.session.updateClipDragUs(Math.round(delta / root.pixelsPerUs));
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
                readonly property var trackBounds: root.trackGeometry(modelData.trackId, true)
                required property var modelData
                objectName: "editDragGhost" + modelData.id
                x: Math.max(scroll.contentX + root.labelWidth, root.visibleX(modelData.startUs))
                y: root.rulerHeight + trackBounds.top
                width: Math.max(0, Math.min(scroll.contentX + scroll.width, root.visibleX(modelData.endUs)) - x)
                height: trackBounds.height
                z: 7
                color: "transparent"
                border.width: 2
                border.color: root.session.dragValid ? ExoTheme.accent : ExoTheme.warning
                Accessible.ignored: true
                Label {
                    anchors.centerIn: parent
                    text: root.session.dragValid ? root.session.formatTimestamp(parent.modelData.startMs) : qsTr("Unavailable")
                    color: root.session.dragValid ? ExoTheme.accent : ExoTheme.warning
                }
                Rectangle { x: root.positionAtUs(clipGhost.modelData.startUs + clipGhost.modelData.fadeInUs) - clipGhost.x; visible: x >= 0 && x <= clipGhost.width - width; y: 0; width: 2; height: 12; color: ExoTheme.accent; Accessible.ignored: true }
                Rectangle { x: root.positionAtUs(clipGhost.modelData.endUs - clipGhost.modelData.fadeOutUs) - clipGhost.x - 2; visible: x >= 0 && x <= clipGhost.width - width; y: 0; width: 2; height: 12; color: ExoTheme.accent; Accessible.ignored: true }
            }
        }
        Repeater {
            model: root.session.dragActive ? root.visibleCandidates(root.session.dragSpacers) : []
            delegate: Rectangle {
                required property var modelData
                objectName: "editSpacerGhost" + modelData.id
                readonly property var trackBounds: root.trackGeometry(modelData.trackId, true)
                x: Math.max(scroll.contentX + root.labelWidth, root.visibleX(modelData.startUs))
                y: root.rulerHeight + trackBounds.top
                width: Math.max(0, Math.min(scroll.contentX + scroll.width, root.visibleX(modelData.endUs)) - x)
                height: trackBounds.height
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
                readonly property var trackBounds: root.trackGeometry(modelData.trackId, true)
                x: Math.max(scroll.contentX + root.labelWidth, root.visibleX(modelData.startUs))
                y: root.rulerHeight + trackBounds.top + root.trackLayout.headerHeight
                width: Math.max(0, Math.min(scroll.contentX + scroll.width, root.visibleX(modelData.endUs)) - x)
                height: Math.min(24, Math.max(0, trackBounds.height - root.trackLayout.headerHeight))
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
                readonly property real audioTop: root.trackLayout.rows.find(value => !value.video)?.top ?? root.trackLayout.baseHeight
                objectName: "editNewTrackDrop" + modelData
                x: scroll.contentX + root.labelWidth
                y: modelData === 0 ? root.rulerHeight - 8 : modelData === 1 ? root.rulerHeight + audioTop - 4 : root.rulerHeight + root.trackLayout.baseHeight
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
            x: root.visibleX(root.session.dragClips[0]?.startUs ?? 0)
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
                readonly property var trackBounds: root.trackGeometry(modelData.trackId, false)
                required property var modelData
                objectName: "editCrossfadeOverlay"
                x: root.visibleX(modelData.startUs)
                y: root.rulerHeight + trackBounds.top + root.trackLayout.headerHeight
                width: root.visibleSpan(modelData.startUs, modelData.endUs)
                height: Math.min(24, Math.max(0, trackBounds.height - root.trackLayout.headerHeight))
                clip: true
                z: 3
                radius: 3
                color: modelData.selected ? ExoTheme.accent : ExoTheme.surface
                border.color: ExoTheme.accent
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Crossfade, %1 ms. Enter to adjust duration.").arg(modelData.durationMs)
                function adjust(): void {
                    root.openTransitionPopup(modelData);
                }
                Keys.onReturnPressed: adjust()
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
                        visible: (index === 0 ? transitionOverlay.modelData.startUs >= viewport.startUs : transitionOverlay.modelData.endUs <= viewport.endUs) && transitionOverlay.width > 0
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
                                root.session.updateClipDragUs(Math.round(delta / root.pixelsPerUs));
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
            originUs: viewport.startUs
            viewportX: 0
            viewportWidth: viewport.usableWidth
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
                root.session.requestSeekUs(root.timeAtUs(mouse.x + root.labelWidth));
            }
            onPositionChanged: mouse => {
                if (pressed)
                    root.session.requestSeekUs(root.timeAtUs(mouse.x + root.labelWidth));
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
            x: root.visibleX(root.session.positionUs)
            visible: root.session.positionUs >= viewport.startUs && root.session.positionUs <= viewport.endUs
            y: scroll.contentY
            z: 5
            width: 1
            height: scroll.height
            color: ExoTheme.accent
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: root.activeFocus ? 9 : 5
                height: root.activeFocus ? 6 : 4
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
                const at = root.timeAtUs(drop.x);
                if (drop.formats.indexOf("application/x-exosnap-crossfade") >= 0) {
                    if (root.session.dropCrossfadeUs(at, Math.min(250000, Math.round(10 / root.pixelsPerUs))))
                        drop.acceptProposedAction();
                    return;
                }
                if (drop.formats.indexOf("application/x-exosnap-history") >= 0)
                    root.session.addHistoryUs(drop.getDataAsString("application/x-exosnap-history"), at);
                else if (drop.formats.indexOf("application/x-exosnap-asset") >= 0)
                    root.session.appendAssetUs(Number(drop.getDataAsString("application/x-exosnap-asset")), at);
                else if (drop.hasUrls)
                    root.session.importMediaBatchUs(drop.urls, true, at);
                drop.acceptProposedAction();
            }
        }
    }
    MouseArea {
        id: panSurface
        objectName: "editPanSurface"
        anchors.fill: scroll
        anchors.leftMargin: root.labelWidth
        anchors.rightMargin: 14
        anchors.bottomMargin: 14
        z: 15
        enabled: interaction.spaceHeld || interaction.pointerCaptured
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        onPressed: mouse => interaction.beginPan(mapToItem(null, mouse.x, mouse.y))
        onPositionChanged: mouse => {
            if (pressed)
                interaction.updatePan(mapToItem(null, mouse.x, mouse.y));
        }
        onReleased: interaction.endPan()
        onCanceled: {
            interaction.cancel();
            interaction.endPan();
        }
    }
    ExoScrollBar {
        objectName: "editHorizontalScrollBar"
        anchors.left: parent.left
        anchors.leftMargin: root.labelWidth + 2
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.bottom: parent.bottom
        orientation: Qt.Horizontal
        z: 12
        size: root.session.durationUs > 0 ? Math.min(1, (viewport.endUs - viewport.startUs) / root.session.durationUs) : 1
        position: root.session.durationUs > 0 ? viewport.startUs / root.session.durationUs : 0
        onPositionChanged: {
            if (pressed)
                viewport.panPixels((position * root.session.durationUs - viewport.startUs) * root.pixelsPerUs);
        }
    }
}
