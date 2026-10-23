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
    readonly property EditViewportAdapter view: viewport
    readonly property EditInteractionController interaction: interactor
    property real userHeaderWidth: 168
    readonly property rect laneViewportRect: Qt.rect(scroll.x + labelWidth, scroll.y + rulerHeight, Math.max(0, scroll.width - labelWidth), Math.max(0, scroll.height - rulerHeight))
    readonly property real laneStart: laneViewportRect.x
    readonly property real rangeBandTop: rulerHeight - 8
    readonly property real rangeBandHeight: 8
    readonly property var displayedRange: session.rangeGestureActive ? session.rangeGestureCandidate : session.editRange
    readonly property var ghostRows: session.dragActive ? session.dragClips : []
    readonly property var donorMaps: {
        const maps = {clips: {}, spacers: {}, junctions: {}};
        if (root.session.dragActive) {
            const donors = root.session.dragDonorIds;
            for (const kind of ["clips", "spacers", "junctions"])
                for (const id of donors[kind] || [])
                    maps[kind][String(id)] = true;
        }
        return maps;
    }
    readonly property var donorMap: donorMaps.clips
    signal headerWidthEdited(real width)
    signal exportRequested()
    signal mediaToggleRequested()
    signal inspectorToggleRequested()
    signal inspectorOpenRequested(string fieldName)
    signal shortcutsRequested()
    property alias pixelsPerSecond: viewport.pixelsPerSecond
    readonly property bool snapping: session.snapping
    property real hoveredGroup: 0
    // Pointer tool for empty lane space: "select" draws a marquee, "range"
    // draws a time range. Session state, not part of the document.
    property string tool: "select"
    // Track reorder in progress: the dragged track and its prospective row.
    property var reorderTrackId: 0
    property int reorderIndex: -1
    function focusTrackGrip(id: var): void {
        for (let i = 0; i < trackLabels.count; ++i) {
            const grip = trackLabels.itemAt(i) ? findGrip(trackLabels.itemAt(i), "editTrackGrip" + id) : null;
            if (grip) {
                grip.forceActiveFocus(Qt.TabFocusReason);
                return;
            }
        }
    }
    function findGrip(item: Item, name: string): Item {
        for (const childItem of item.children)
            if (childItem.objectName === name)
                return childItem;
        return null;
    }
    property bool draggingCrossfade: false
    property bool insertDrag: false
    property var dragTrack: 0
    property point pointerPosition: Qt.point(0, 0)
    property var pointerUpdater: null
    property bool pointerScrolling: false
    property bool pointerScrollVertical: true
    function stopPointerScroll(): void {
        pointerScrolling = false;
        pointerUpdater = null;
    }
    function settlePointerCapture(): void {
        if (!root.session.dragActive && !root.session.rangeGestureActive && !root.session.hasPointerSelection()) {
            stopPointerScroll();
            root.capturedClipId = 0;
            root.capturedJunctionId = 0;
            root.capturedSpacerId = 0;
        }
    }
    FrameAnimation {
        running: root.visible && root.pointerScrolling && root.pointerUpdater !== null
        onTriggered: {
            if (!root.session.dragActive && !root.session.rangeGestureActive && !laneMouse.marquee && !root.reorderTrackId &&
                    !mediaDropArea.containsDrag) {
                root.stopPointerScroll();
                return;
            }
            const area = root.laneViewportRect;
            const velocity = root.interaction.edgeAutoScroll(Qt.point(root.pointerPosition.x - area.x,
                root.pointerPosition.y - area.y), Qt.size(area.width, area.height));
            const elapsed = Math.min(0.05, Math.max(0, frameTime));
            if (velocity.x !== 0)
                root.panTimeline(velocity.x * elapsed);
            if (root.pointerScrollVertical && velocity.y !== 0)
                scroll.contentY = Math.max(0, Math.min(scroll.contentHeight - scroll.height,
                    scroll.contentY + velocity.y * elapsed));
            if (velocity.x !== 0 || (root.pointerScrollVertical && velocity.y !== 0))
                root.pointerUpdater(root.pointerPosition);
        }
    }
    property string inputHint: ""
    property var trackToDelete: ({id: 0, name: ""})
    property var trackEditorId: 0
    readonly property var trackEditorRow: root.trackLayout.geometry[String(root.trackEditorId)] ?? ({})
    readonly property real labelWidth: Math.min(width < 1000 ? 96 : 280, Math.max(80, Math.min(280, userHeaderWidth)))
    readonly property real rulerHeight: 26
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
        return (preview ? root.trackLayout.previewGeometry : root.trackLayout.geometry)[String(trackId)] ?? {top: 0, height: 0, headerHeight: 0, contentHeight: 0};
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
    function revealPlacement(): void {
        const bounds = root.session.dragBounds();
        if (!root.session.dragActive || !bounds.valid ||
            (bounds.startUs >= viewport.startUs && bounds.endUs <= viewport.endUs))
            return;
        if (bounds.endUs - bounds.startUs > viewport.endUs - viewport.startUs) {
            viewport.fitSelection(bounds.startUs, bounds.endUs);
        } else {
            viewport.beginViewOperation();
            const delta = bounds.startUs < viewport.startUs ? bounds.startUs - viewport.startUs : bounds.endUs - viewport.endUs;
            viewport.panPixels(delta * root.pixelsPerUs);
            viewport.endViewOperation();
        }
    }
    function panTimeline(pixels: real): void {
        viewport.panPixels(pixels);
    }
    function previewDrag(delta: real, contentY: real, insert: bool): void {
        root.insertDrag = insert;
        const destination = root.trackLayout.dropTargetAtY(contentY - root.rulerHeight);
        if ((destination.newTrack ?? -1) < 0) {
            const track = root.interaction.dragTrackId(root.trackLayout.rows, root.dragTrack, contentY - root.rulerHeight);
            if (track)
                root.dragTrack = track;
        }
        root.session.updateClipDragUs(Math.round(delta), (destination.newTrack ?? -1) < 0 ? root.dragTrack : 0,
            destination.newTrack ?? -1, root.insertDrag, Math.round(6 / root.pixelsPerUs));
    }
    property bool visualWorkPending: false
    property bool clipSyncPending: false
    property var capturedClipId: 0
    property var capturedJunctionId: 0
    property var capturedSpacerId: 0
    onCapturedClipIdChanged: scheduleClipSync()
    onCapturedJunctionIdChanged: scheduleClipSync()
    onCapturedSpacerIdChanged: scheduleClipSync()
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
        const tracks = root.trackLayout.trackIdsIntersecting(scroll.contentY,
            scroll.contentY + Math.max(0, scroll.height - root.rulerHeight));
        clipModel.syncViewportUs(root.session, viewport.startUs, viewport.endUs,
            tracks, root.capturedClipId ? [root.capturedClipId] : []);
        spacerModel.setRows(visibleCandidates(root.session.spacers, false));
        candidateClipModel.setRows(visibleCandidates(root.ghostRows, true));
        candidateSpacerModel.setRows(root.session.dragActive ? visibleCandidates(root.session.dragSpacers, true) : []);
        candidateJunctionModel.setRows(root.session.dragActive ? visibleCandidates(root.session.dragTransitions, true) : []);
        junctionModel.setRows(tracks.length > 0 || root.capturedJunctionId ? root.session.visibleTransitionsUs(viewport.startUs, viewport.endUs, tracks, true, root.capturedJunctionId ? [root.capturedJunctionId] : []) : []);
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
    Connections {
        target: root.session
        function onSourceVerificationChanged(asset: real): void { root.scheduleClipSync(); }
        function onPositionChanged(): void { root.scheduleVisualWork(); }
    }
    Connections {
        target: root.thumbnails
        function onSourcesChanged(): void { root.scheduleVisualWork(); }
    }
    function visibleCandidates(rows: var, preview: bool): var {
        const bottom = scroll.contentY + Math.max(0, scroll.height - root.rulerHeight);
        return rows.filter(value => {
            if (!preview && value.id === root.capturedSpacerId)
                return true;
            const track = root.trackGeometry(value.trackId, preview);
            const intersects = value.startUs === value.endUs
                ? value.startUs >= viewport.startUs && value.startUs <= viewport.endUs
                : value.startUs < viewport.endUs && value.endUs > viewport.startUs;
            return intersects &&
                track.height > 0 && track.top < bottom && track.top + track.height > scroll.contentY;
        });
    }
    Component.onCompleted: {
        trackLabelModel.setRows(root.trackLayout.rows);
        refreshClips();
    }
    EditViewportAdapter {
        id: viewport
        objectName: "editViewport"
        timelineEndUs: Math.max(root.session.durationUs, root.session.dragActive ? root.session.dragDurationUs : 0)
        usableWidth: Math.max(1, scroll.width - root.labelWidth)
        onViewChanged: root.scheduleClipSync()
    }
    EditInteractionController {
        id: interactor
        objectName: "editInteraction"
        scopeItem: root.keyboardScope
        blocked: root.session.dragActive || root.session.rangeGestureActive || root.player.scrubbing || !root.visible || !root.enabled
        onTogglePlayRequested: root.player.togglePlay()
        onRangeOnceRequested: root.player.playRangeOnce()
        onPanRequested: pixels => root.panTimeline(pixels)
    }
    EditTimelineClipModel {
        id: clipModel
        objectName: "editVisibleClips"
    }
    EditTimelineClipModel { id: trackLabelModel }
    EditTimelineClipModel { id: junctionModel; objectName: "editVisibleJunctions" }
    EditTimelineClipModel { id: spacerModel }
    EditTimelineClipModel { id: candidateClipModel }
    EditTimelineClipModel { id: candidateSpacerModel }
    EditTimelineClipModel { id: candidateJunctionModel }
    readonly property real pixelsPerMs: pixelsPerSecond / 1000
    readonly property real pixelsPerUs: pixelsPerSecond / 1000000
    objectName: "editClipTimeline"
    activeFocusOnTab: true
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Edit timeline")
    Keys.priority: Keys.AfterItem
    Keys.onPressed: event => {
        const focused = root.Window.window ? root.Window.window.activeFocusItem : null;
        if (focused instanceof TextInput || focused instanceof TextEdit)
            return;
        if (event.key === Qt.Key_Escape && laneMouse.marquee) {
            laneMouse.cancelMarquee();
            event.accepted = true;
            return;
        }
        if (event.key === Qt.Key_Escape && root.reorderTrackId) {
            root.stopPointerScroll();
            root.reorderTrackId = 0;
            event.accepted = true;
            return;
        }
        if (event.key === Qt.Key_Escape && (root.session.dragActive || root.session.rangeGestureActive || root.session.hasPointerSelection() || root.capturedSpacerId)) {
            root.stopPointerScroll();
            root.capturedClipId = 0;
            root.capturedJunctionId = 0;
            root.capturedSpacerId = 0;
            root.session.cancelClipDrag();
            root.session.cancelRangeGesture();
            root.session.finishPointerSelection(true);
            event.accepted = true;
            return;
        }
        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && root.session.dragActive) {
            root.stopPointerScroll();
            root.capturedClipId = 0;
            root.capturedJunctionId = 0;
            root.capturedSpacerId = 0;
            root.session.commitClipDrag();
            event.accepted = true;
            return;
        }
        if (event.key === Qt.Key_Space) {
            event.accepted = root.interaction.armSpace(event.modifiers, event.isAutoRepeat);
            return;
        }
        const action = root.interaction.keyAction(event.key, event.modifiers, event.isAutoRepeat);
        if (action.length === 0)
            return;
        switch (action) {
        case "zoom-in": root.zoomTo(root.pixelsPerSecond * 1.25); break;
        case "zoom-out": root.zoomTo(root.pixelsPerSecond / 1.25); break;
        case "fit-timeline": root.view.fitTimeline(); break;
        case "fit-selection": {
            const bounds = root.session.selectionBounds();
            if (bounds.valid)
                root.view.fitSelection(bounds.startUs, bounds.endUs);
            break;
        }
        case "previous-view": root.view.restorePreviousView(); break;
        case "loop": root.player.loopEnabled = !root.player.loopEnabled; break;
        case "media-dock": root.mediaToggleRequested(); break;
        case "inspector-dock": root.inspectorToggleRequested(); break;
        case "export": root.exportRequested(); break;
        case "shortcuts": root.shortcutsRequested(); break;
        case "tool-select": root.tool = "select"; break;
        case "tool-range": root.tool = "range"; break;
        case "context-menu":
            if (root.session.selectedClip !== 0)
                clipMenu.popup();
            else if (root.session.selectionMode === EditSessionAdapter.GapSelection)
                gapMenu.popup();
            else if (root.session.selectedTransition !== 0)
                root.openTransitionPopup(root.session.selectedJunctionDetails);
            break;
        case "cancel":
            if (root.session.selectionMode === EditSessionAdapter.RangeSelection)
                root.session.clearEditRange();
            else
                root.session.clearSelection();
            break;
        case "paste":
        case "insert-paste":
            if (root.session.executeTimelineAction(action))
                Qt.callLater(root.revealPlacement);
            break;
        default: root.session.executeTimelineAction(action); break;
        }
        event.accepted = true;
    }
    // Opens the junction popup for one transition row, as Enter on its overlay
    // does.
    function openTransitionPopup(row: var): void {
        if (!root.session.preflightPendingDraft())
            return;
        root.session.selectTransition(row.outgoing);
        transitionDuration.junctionId = row.id;
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
            objectName: "editLinkSelection"
            text: qsTr("Link selection")
            enabled: root.session.selectedClipIds.length >= 2
            onTriggered: root.session.linkSelectedClips()
        }
        ExoMenuItem {
            objectName: "editUnlinkSelection"
            text: qsTr("Unlink")
            enabled: root.session.selectedClipIds.length > 0
            onTriggered: root.session.unlinkSelectedClips()
        }
        ExoMenuItem {
            objectName: "editCrossfadeAtCut"
            text: qsTr("Crossfade to next clip")
            enabled: clipMenu.visible && root.session.selectedClip !== 0 && root.session.canCrossfadeAtCut()
            onTriggered: {
                root.session.previewCrossfadeAtCut();
                root.forceActiveFocus();
            }
        }
        ExoMenuItem {
            objectName: "editJunctionBeforeClip"
            text: qsTr("Junction before clip")
            visible: (root.session.selectedClipDetails.incomingJunctionOutgoing ?? 0) !== 0
            onTriggered: {
                const outgoing = root.session.selectedClipDetails.incomingJunctionOutgoing;
                const row = root.session.transitions.find(item => item.outgoing === outgoing);
                if (row)
                    root.openTransitionPopup(row);
            }
        }
        ExoMenuItem {
            objectName: "editJunctionAfterClip"
            text: qsTr("Junction after clip")
            visible: (root.session.selectedClipDetails.outgoingJunctionOutgoing ?? 0) !== 0
            onTriggered: {
                const outgoing = root.session.selectedClipDetails.outgoingJunctionOutgoing;
                const row = root.session.transitions.find(item => item.outgoing === outgoing);
                if (row)
                    root.openTransitionPopup(row);
            }
        }
    }
    Popup {
        id: transitionDuration
        objectName: "editCrossfadeDurationPopup"
        property var junctionId: 0
        readonly property var details: root.session.transitions.find(row => row.id === junctionId) ?? ({})
        readonly property bool crossfade: details.kind === "crossfade"
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
        contentItem: ExoScrollView {
            id: junctionScroll
            objectName: "editJunctionPopupScroll"
            reserveScrollBarGutters: true
            bottomPadding: 0
            implicitHeight: Math.min(contentHeight + topPadding + bottomPadding, Math.max(0, (Overlay.overlay?.height ?? root.height) - 16 - transitionDuration.topPadding - transitionDuration.bottomPadding))
            contentWidth: availableWidth
            contentHeight: junctionFields.implicitHeight
            clip: true
            function revealFocusedItem(): void {
                if (!transitionDuration.opened)
                    return;
                const item = root.Window.window?.activeFocusItem;
                let owner = item;
                while (owner && owner !== junctionFields)
                    owner = owner.parent;
                if (owner === junctionFields)
                    junctionScroll.revealItem(item);
            }
            Connections {
                target: transitionDuration.opened ? root.Window.window : null
                function onActiveFocusItemChanged(): void {
                    Qt.callLater(junctionScroll.revealFocusedItem);
                }
            }
            Column {
                id: junctionFields
                width: junctionScroll.availableWidth
                spacing: ExoTheme.spacingSm
                Label {
                    objectName: "editJunctionPopupTitle"
                    text: transitionDuration.crossfade ? qsTr("Crossfade") : qsTr("Hard cut")
                    color: ExoTheme.text
                    font.family: ExoTheme.sansFamily
                    font.pixelSize: ExoTheme.fontSectionTitle
                    font.weight: Font.DemiBold
                }
                ExoSettingRow {
                    width: parent.width
                    visible: transitionDuration.crossfade
                    label: qsTr("Duration")
                    stacked: true
                    EditTimeField {
                        id: durationInput
                        objectName: "editCrossfadeDuration"
                        Layout.fillWidth: true
                        session: root.session
                        targetId: transitionDuration.junctionId
                        valueUs: transitionDuration.details.durationUs ?? 0
                        sampleContext: transitionDuration.details.video === false && root.session.timeValueUsesSamples(targetId, "junctionDuration")
                        canvasFocusItem: transitionDuration.contentItem
                        applyValue: (value, target) => root.session.applyTimeValue(target, "junctionDuration", value)
                        Accessible.name: qsTr("Crossfade duration")
                    }
                }
                ExoSettingRow {
                    width: parent.width
                    visible: transitionDuration.crossfade
                    label: qsTr("Start")
                    stacked: true
                    EditTimeField {
                        id: windowStart
                        objectName: "editJunctionStart"
                        Layout.fillWidth: true
                        session: root.session
                        targetId: transitionDuration.junctionId
                        valueUs: transitionDuration.details.startUs ?? 0
                        sampleContext: transitionDuration.details.video === false && root.session.timeValueUsesSamples(targetId, "junctionStart")
                        canvasFocusItem: transitionDuration.contentItem
                        applyValue: (value, target) => root.session.applyTimeValue(target, "junctionStart", value)
                        Accessible.name: qsTr("Junction start")
                    }
                }
                ExoSettingRow {
                    width: parent.width
                    visible: transitionDuration.crossfade
                    label: qsTr("End")
                    stacked: true
                    EditTimeField {
                        id: windowEnd
                        objectName: "editJunctionEnd"
                        Layout.fillWidth: true
                        session: root.session
                        targetId: transitionDuration.junctionId
                        valueUs: transitionDuration.details.endUs ?? 0
                        sampleContext: transitionDuration.details.video === false && root.session.timeValueUsesSamples(targetId, "junctionEnd")
                        canvasFocusItem: transitionDuration.contentItem
                        applyValue: (value, target) => root.session.applyTimeValue(target, "junctionEnd", value)
                        Accessible.name: qsTr("Junction end")
                    }
                }
                ExoSettingRow {
                    width: parent.width
                    visible: transitionDuration.crossfade
                    label: qsTr("Curve")
                    stacked: true
                    ExoSelect {
                        id: curveInput
                        objectName: "editJunctionCurve"
                        Layout.fillWidth: true
                        options: [
                            { value: 0, label: qsTr("Linear"), selectable: true },
                            { value: 1, label: qsTr("Smooth"), selectable: true },
                            { value: 2, label: qsTr("Bias"), selectable: true }
                        ]
                        value: root.session.selectedJunctionDetails.curve ?? 0
                        Accessible.name: qsTr("Junction curve")
                        onValueActivated: value => root.session.setJunctionCurveFor(transitionDuration.junctionId, value, transitionDuration.details.bias ?? 0)
                    }
                }
                ExoSettingRow {
                    width: parent.width
                    visible: transitionDuration.crossfade
                    label: qsTr("Curve bias")
                    stacked: true
                    EditDraftField {
                        id: biasInput
                        objectName: "editJunctionBias"
                        Layout.fillWidth: true
                        session: root.session
                        canvasFocusItem: transitionDuration.contentItem
                        targetId: transitionDuration.junctionId
                        value: transitionDuration.details.bias ?? 0
                        context: ({curve: transitionDuration.details.curve ?? 0})
                        formatValue: value => String(Math.round(value * 10000) / 100)
                        parseValue: text => root.session.parseCurveBiasValue(text)
                        applyValue: (value, target, context) => root.session.setJunctionCurveFor(target, context.curve, value)
                        Accessible.name: qsTr("Curve bias")
                        Accessible.description: errorText.length > 0 ? errorText : qsTr("Curve bias from -100% to 100%")
                    }
                }
                ExoSettingRow {
                    id: lawRow
                    width: parent.width
                    visible: transitionDuration.crossfade && !transitionDuration.details.video
                    label: qsTr("Audio gain law")
                    stacked: true
                    ExoSelect {
                        id: lawInput
                        objectName: "editJunctionAudioLaw"
                        Layout.fillWidth: true
                        options: [
                            { value: 0, label: qsTr("Equal Gain"), selectable: true },
                            { value: 1, label: qsTr("Equal Power"), selectable: true }
                        ]
                        value: root.session.selectedJunctionDetails.audioLaw ?? 0
                        Accessible.name: qsTr("Audio gain law")
                        onValueActivated: value => root.session.setJunctionAudioLaw(value)
                    }
                }
                ExoSettingRow {
                    width: parent.width
                    visible: !transitionDuration.crossfade
                    label: qsTr("Cut position")
                    stacked: true
                    EditTimeField {
                        id: cutInput
                        objectName: "editJunctionCut"
                        Layout.fillWidth: true
                        session: root.session
                        targetId: transitionDuration.junctionId
                        valueUs: transitionDuration.details.cutUs ?? 0
                        sampleContext: transitionDuration.details.video === false && root.session.timeValueUsesSamples(targetId, "junctionCut")
                        canvasFocusItem: transitionDuration.contentItem
                        applyValue: (value, target) => root.session.applyTimeValue(target, "junctionCut", value)
                        Accessible.name: qsTr("Cut position")
                    }
                }
                ExoButton {
                    objectName: "editRemoveCrossfade"
                    width: parent.width
                    visible: transitionDuration.crossfade
                    text: qsTr("Convert to hard cut")
                    onClicked: {
                        root.session.removeTransition();
                        transitionDuration.close();
                    }
                }
                ExoButton {
                    objectName: "editRemoveOverlap"
                    width: parent.width
                    text: qsTr("Remove overlap (preview)")
                    onClicked: {
                        if (root.session.previewRemoveOverlap(transitionDuration.junctionId)) {
                            transitionDuration.close();
                            root.forceActiveFocus();
                        }
                    }
                }
            }
        }
        onOpened: {
            junctionScroll.contentItem.contentY = 0;
            if (crossfade)
                durationInput.forceActiveFocus(Qt.PopupFocusReason);
            else
                cutInput.forceActiveFocus(Qt.PopupFocusReason);
            Qt.callLater(junctionScroll.revealFocusedItem);
        }
        onClosed: {
            durationInput.cancelPendingDraft("");
            windowStart.cancelPendingDraft("");
            windowEnd.cancelPendingDraft("");
            cutInput.cancelPendingDraft("");
            biasInput.cancelPendingDraft("");
        }
    }
    ToolTip.visible: root.inputHint.length > 0 && activeFocus
    ToolTip.text: root.inputHint
    ToolTip.delay: 1000
    ToolTip.timeout: 4000
    Popup {
        id: trackEditor
        objectName: "editTrackSettings"
        width: 300
        padding: 12
        modal: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle { color: ExoTheme.surfaceRaised; border.color: ExoTheme.lineStrong; radius: ExoTheme.radiusMd }
        ColumnLayout {
            width: parent.width
            Label { text: qsTr("Track settings"); color: ExoTheme.text }
            Label { text: qsTr("Name"); color: ExoTheme.textSecondary }
            EditDraftField {
                id: trackName
                objectName: "editTrackName"
                Layout.fillWidth: true
                session: root.session
                canvasFocusItem: root
                targetId: root.trackEditorId
                context: root.trackEditorRow.name ?? ""
                formatValue: (value, context) => context
                parseValue: text => ({valid: text.trim().length > 0, value: text.trim(), error: qsTr("Enter a track name.")})
                applyValue: (value, target) => root.session.setTrackName(target, value)
                Accessible.name: qsTr("Track name")
            }
            Label { visible: !root.trackEditorRow.video; text: qsTr("Track gain (dB)"); color: ExoTheme.textSecondary }
            EditGainField {
                id: trackGain
                objectName: "editTrackGain"
                Layout.fillWidth: true
                visible: !root.trackEditorRow.video
                enabled: !root.trackEditorRow.locked
                session: root.session
                canvasFocusItem: root
                targetId: root.trackEditorId
                track: true
                gainDb: root.trackEditorRow.gainDb ?? 0
                applyValue: (value, target) => root.session.setTrackGain(target, value)
                Accessible.name: qsTr("Track gain in decibels")
            }
            Label { text: qsTr("Height"); color: ExoTheme.textSecondary }
            RowLayout {
                Repeater {
                    model: [22, 44, 64, 112]
                    delegate: ExoButton {
                        required property int modelData
                        text: String(modelData)
                        compact: true
                        onClicked: if (root.session.preflightPendingDraft()) root.trackLayout.setTrackHeight(root.trackEditorId, modelData)
                        Accessible.name: qsTr("Track height %1 pixels").arg(modelData)
                    }
                }
            }
            Label { text: qsTr("Color"); color: ExoTheme.textSecondary }
            RowLayout {
                Repeater {
                    model: ["#738d9c", "#a18a6a", "#79957b", "#9c7a87", "#9184a3", ""]
                    delegate: ExoButton {
                        required property string modelData
                        implicitWidth: 34
                        compact: true
                        text: modelData.length ? "●" : "×"
                        contentItem: Label { text: parent.text; color: parent.modelData.length ? parent.modelData : ExoTheme.text; horizontalAlignment: Text.AlignHCenter }
                        onClicked: if (root.session.preflightPendingDraft()) root.trackLayout.setTrackColor(root.trackEditorId, modelData)
                        Accessible.name: modelData.length ? qsTr("Track color %1").arg(modelData) : qsTr("Default track color")
                    }
                }
            }
        }
        onClosed: {
            trackName.cancelPendingDraft("");
            trackGain.cancelPendingDraft("");
        }
    }
    Connections {
        target: root.session
        function onDragChanged(): void { root.settlePointerCapture(); root.scheduleClipSync(); }
        function onRangeGestureChanged(): void { root.settlePointerCapture(); }
        function onWorkspaceChanged(): void {
            root.refreshClips();
        }
        function onInteractionSettingsChanged(): void { root.refreshClips(); }
    }
    Connections {
        target: root.trackLayout
        function onLayoutChanged(): void {
            trackLabelModel.setRows(root.trackLayout.rows);
            root.scheduleClipSync();
        }
    }
    ExoButton {
        id: applyPreviewButton
        objectName: "editApplyGapPreview"
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: ExoTheme.spacingSm
        z: 20
        tone: "primary"
        compact: true
        visible: root.session.dragActive && (root.session.selectedGap.endMs !== undefined || root.capturedClipId === 0)
        enabled: root.session.dragValid
        text: root.session.dragValid ? qsTr("Apply preview (Enter)") : qsTr("Preview unavailable")
        onClicked: root.session.commitClipDrag()
        ToolTip.text: qsTr("Review all outlined clips. Enter applies; Escape cancels.")
        ToolTip.visible: hovered
    }
    ExoButton {
        objectName: "editCancelPreview"
        anchors.right: applyPreviewButton.left
        anchors.top: applyPreviewButton.top
        anchors.rightMargin: ExoTheme.spacingSm
        z: 20
        compact: true
        visible: applyPreviewButton.visible
        text: qsTr("Cancel")
        onClicked: root.session.cancelClipDrag()
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
        onContentYChanged: root.scheduleClipSync()
        onHeightChanged: root.scheduleClipSync()
        ScrollBar.vertical: ExoScrollBar {
            objectName: "editVerticalScrollBar"
            policy: ScrollBar.AsNeeded
        }
        WheelHandler {
            acceptedModifiers: Qt.KeyboardModifierMask
            activeTimeout: 0.3
            onActiveChanged: if (!active) viewport.endViewOperation()
            onWheel: event => {
                const viewportPoint = scroll.contentItem.mapToItem(scroll, event.x, event.y);
                const action = interaction.handleWheel(event.modifiers, event.pixelDelta, event.angleDelta, Qt.NoScrollPhase);
                if (action.zoom !== 0)
                    viewport.zoomWheelAt(root.pixelsPerSecond * Math.pow(1.25, action.zoom), Math.max(0, viewportPoint.x - root.labelWidth));
                if (action.pan !== 0)
                    root.panTimeline(action.pan);
                if (action.scroll !== 0)
                    scroll.contentY = Math.max(0, Math.min(scroll.contentHeight - scroll.height, scroll.contentY + action.scroll));
                if (action.height !== 0) {
                    const track = root.trackLayout.trackAtY(viewportPoint.y + scroll.contentY - root.rulerHeight);
                    if (track)
                        root.trackLayout.setTrackHeight(track, root.trackLayout.heightForId(track) + action.height);
                }
                if (action.finished)
                    viewport.endViewOperation();
                event.accepted = true;
            }
        }
        MouseArea {
            id: laneMouse
            objectName: "editLanePointer"
            preventStealing: true
            width: scroll.contentWidth
            height: scroll.contentHeight
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            property point pressPoint: Qt.point(0, 0)
            property real pressTimeUs: 0
            property var pressTrack: 0
            property var pressPolicy: ({valid: false})
            property bool started: false
            // Marquee state. The selection before the press is kept so Escape
            // can restore it; the playhead too, because a plain press seeks.
            property bool marquee: false
            property bool marqueeArmed: false
            property var marqueeOriginal: []
            property real marqueeStartX: 0
            property real marqueeStartY: 0
            property real marqueeEndX: 0
            property real marqueeEndY: 0
            property real pressPlayheadUs: 0
            function cancelMarquee(): void {
                marquee = false;
                marqueeArmed = false;
                pressPolicy = {valid: false};
                root.stopPointerScroll();
                root.session.finishPointerSelection(true);
            }
            function updateMarquee(local: point): void {
                marqueeEndX = local.x;
                marqueeEndY = local.y;
                const rows = root.trackLayout.rows;
                const from = rows.findIndex(row => row.id === pressTrack);
                const current = root.trackLayout.trackAtY(Math.max(0, local.y - root.rulerHeight));
                const to = current ? rows.findIndex(row => row.id === current) : local.y < marqueeStartY ? 0 : rows.length - 1;
                const tracks = rows.slice(Math.min(from, to), Math.max(from, to) + 1).map(row => row.id);
                root.session.selectClipArea(pressTimeUs, root.timeAtUs(local.x), tracks,
                    pressPolicy.additive ? marqueeOriginal : [], pressPolicy.exact === true);
            }
            onPressed: mouse => {
                pressPolicy = {valid: false};
                marquee = false;
                marqueeArmed = false;
                if (!root.session.preflightPendingDraft())
                    return;
                pressTrack = root.trackLayout.trackAtY(mouse.y - root.rulerHeight);
                if (!pressTrack)
                    return;
                if (mouse.x < root.labelWidth) {
                    root.session.setActiveTrack(pressTrack);
                    root.forceActiveFocus();
                    return;
                }
                pressTimeUs = root.timeAtUs(mouse.x);
                pressPoint = mapToItem(root, mouse.x, mouse.y);
                started = false;
                if (mouse.button === Qt.RightButton) {
                    root.session.selectGapUs(pressTrack, pressTimeUs);
                    root.forceActiveFocus();
                    if (root.session.selectedGap.endUs !== undefined)
                        gapMenu.popup();
                    return;
                }
                const hasRange = root.session.selectionMode === EditSessionAdapter.RangeSelection;
                pressPolicy = root.interaction.clipPointerPolicy(EditInteractionController.PointerContent, mouse.modifiers, hasRange);
                // The selection tool turns a drag from empty space into a marquee:
                // Ctrl adds to the selection, Alt ignores link groups. The range
                // tool keeps the time-range gestures, plain drag included.
                if (root.tool === "select" && (mouse.modifiers === Qt.ControlModifier || mouse.modifiers === Qt.AltModifier))
                    pressPolicy = {valid: true, range: false, seek: false, additive: mouse.modifiers === Qt.ControlModifier,
                        exact: mouse.modifiers === Qt.AltModifier, error: ""};
                else if (root.tool === "range" && mouse.modifiers === Qt.NoModifier)
                    pressPolicy = {valid: true, range: true, seek: false, extend: false, anchorAtPlayhead: false,
                        preserveBounds: false, error: ""};
                root.inputHint = pressPolicy.error;
                if (!pressPolicy.valid)
                    return;
                marqueeArmed = !pressPolicy.range;
                marqueeOriginal = root.session.selectedClipIds;
                marqueeStartX = marqueeEndX = mouse.x;
                marqueeStartY = marqueeEndY = mouse.y;
                pressPlayheadUs = root.session.positionUs;
                if (pressPolicy.range) {
                    const anchor = pressPolicy.anchorAtPlayhead ? root.session.positionUs : pressTimeUs;
                    root.session.beginRangeGestureUs(anchor, [pressTrack], pressPolicy.extend, pressPolicy.preserveBounds);
                    root.session.updateRangeGestureToTrackUs(pressTimeUs, pressTrack);
                } else if (marqueeArmed || pressPolicy.seek) {
                    root.session.beginLanePointerSelection(pressTrack, pressTimeUs, pressPolicy.seek === true);
                }
                root.pointerPosition = pressPoint;
                root.pointerScrollVertical = true;
                root.pointerUpdater = physical => laneMouse.updatePointer(physical);
                root.forceActiveFocus();
            }
            function updatePointer(point: point): void {
                if (!pressed || !(pressedButtons & Qt.LeftButton) || !pressPolicy.valid ||
                        (!marqueeArmed && !root.session.hasPointerSelection() && !root.session.rangeGestureActive))
                    return;
                const local = scroll.contentItem.mapFromItem(root, point.x, point.y);
                if (!started && root.interaction.clipPointerThreshold(EditInteractionController.PointerContent,
                        point.x - pressPoint.x, point.y - pressPoint.y, Qt.styleHints.startDragDistance)) {
                    started = true;
                    if (marqueeArmed) {
                        // The press already seeked and picked a gap; a drag means
                        // neither, so both are undone before the marquee begins.
                        root.session.finishPointerSelection(true);
                        root.session.beginLanePointerSelection(pressTrack, pressPlayheadUs, false);
                        if (root.session.positionUs !== pressPlayheadUs)
                            root.session.requestSeekUs(pressPlayheadUs);
                        marquee = true;
                    }
                }
                if (started && marquee) {
                    root.pointerScrolling = true;
                    updateMarquee(local);
                } else if (started) {
                    root.pointerScrolling = true;
                    const track = root.trackLayout.trackAtY(local.y - root.rulerHeight);
                    root.session.updateRangeGestureToTrackUs(root.timeAtUs(local.x), track || pressTrack);
                }
            }
            onPositionChanged: mouse => {
                root.pointerPosition = mapToItem(root, mouse.x, mouse.y);
                updatePointer(root.pointerPosition);
            }
            onReleased: mouse => {
                root.stopPointerScroll();
                if (mouse.button !== Qt.LeftButton || !pressPolicy.valid)
                    return;
                if (root.session.rangeGestureActive) {
                    if (started || (pressPolicy.range && pressPolicy.extend))
                        root.session.commitRangeGesture();
                    else
                        root.session.cancelRangeGesture();
                }
                root.session.finishPointerSelection(false, started);
                if (pressPolicy.seek && !marquee)
                    root.session.finishPointerSeek();
                marquee = false;
                marqueeArmed = false;
            }
            onCanceled: {
                root.stopPointerScroll();
                if (marquee)
                    cancelMarquee();
                root.session.cancelRangeGesture();
                root.session.finishPointerSelection(true);
            }
            Rectangle {
                objectName: "editMarquee"
                // Above the clips, which are later siblings of the lane pointer.
                parent: scroll.contentItem
                visible: laneMouse.marquee
                x: laneMouse.x + Math.min(laneMouse.marqueeStartX, laneMouse.marqueeEndX)
                y: laneMouse.y + Math.min(laneMouse.marqueeStartY, laneMouse.marqueeEndY)
                width: Math.abs(laneMouse.marqueeEndX - laneMouse.marqueeStartX)
                height: Math.abs(laneMouse.marqueeEndY - laneMouse.marqueeStartY)
                z: 10
                color: Qt.rgba(ExoTheme.accent.r, ExoTheme.accent.g, ExoTheme.accent.b, 0.08)
                border.width: 1
                border.color: ExoTheme.accent
                Accessible.ignored: true
            }
            onDoubleClicked: {
                const gap = root.session.selectedGap;
                if (gap.endUs > gap.startUs)
                    root.session.setEditRangeUs(gap.startUs, gap.endUs, [gap.trackId]);
            }
        }
        Repeater {
            id: trackLabels
            model: trackLabelModel
            delegate: Rectangle {
                id: trackLabel
                required property var modelData
                required property int index
                objectName: "editTrackLabel" + index
                x: scroll.contentX
                y: root.rulerHeight + modelData.top
                width: root.labelWidth
                height: modelData.height
                z: 3
                color: ExoTheme.surface
                Rectangle {
                    anchors.fill: parent
                    color: trackLabel.modelData.color || "transparent"
                    opacity: 0.22
                    Accessible.ignored: true
                }
                Label {
                    x: 18
                    y: 2
                    width: Math.max(0, parent.width - x - 31)
                    text: trackLabel.modelData.typeName + " " + trackLabel.modelData.name
                    color: ExoTheme.text
                    font.pixelSize: ExoTheme.fontCaption
                    elide: Text.ElideRight
                }
                MouseArea {
                    id: trackHeader
                    x: 16
                    y: 0
                    width: Math.max(0, parent.width - 48)
                    height: 18
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    preventStealing: true
                    onPressed: mouse => {
                        if (!root.session.preflightPendingDraft()) mouse.accepted = false;
                    }
                    onClicked: mouse => {
                        if (!root.session.preflightPendingDraft())
                            return;
                        root.session.setActiveTrack(trackLabel.modelData.id);
                        if (mouse.button === Qt.RightButton)
                            trackMenu.popup();
                    }
                    onDoubleClicked: {
                        if (!root.session.preflightPendingDraft())
                            return;
                        root.trackLayout.toggleCollapsed(trackLabel.modelData.id);
                    }
                }
                ExoButton {
                    objectName: "editTrackMenu" + trackLabel.modelData.id
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    width: 24
                    height: 22
                    compact: true
                    text: "⋮"
                    Accessible.name: qsTr("Track actions")
                    onClicked: if (root.session.preflightPendingDraft()) trackMenu.popup()
                }
                Row {
                    x: 18
                    y: 18
                    height: 22
                    visible: trackLabel.height >= 44
                    spacing: 2
                    ExoButton {
                        objectName: "editTrackEnabled" + trackLabel.modelData.id
                        visible: root.labelWidth >= 120
                        width: 30
                        height: 22
                        compact: true
                        text: trackLabel.modelData.video ? (trackLabel.modelData.enabled ? "V" : "×") : "M"
                        highlighted: !trackLabel.modelData.enabled
                        enabled: !trackLabel.modelData.locked
                        Accessible.name: trackLabel.modelData.video ? qsTr("Video visibility") : qsTr("Track mute")
                        Accessible.checkable: true
                        Accessible.checked: trackLabel.modelData.video ? trackLabel.modelData.enabled : !trackLabel.modelData.enabled
                        onClicked: root.session.setTrackEnabled(trackLabel.modelData.id, !trackLabel.modelData.enabled)
                    }
                    ExoButton {
                        objectName: "editTrackSolo" + trackLabel.modelData.id
                        visible: !trackLabel.modelData.video && root.labelWidth >= 120
                        width: 30
                        height: 22
                        compact: true
                        text: "S"
                        highlighted: trackLabel.modelData.solo
                        Accessible.name: qsTr("Track solo")
                        Accessible.checkable: true
                        Accessible.checked: trackLabel.modelData.solo
                        onClicked: root.session.setTrackSolo(trackLabel.modelData.id, !trackLabel.modelData.solo)
                    }
                    ExoButton {
                        objectName: "editTrackLock" + trackLabel.modelData.id
                        visible: root.labelWidth >= 168
                        width: 30
                        height: 22
                        compact: true
                        text: "L"
                        highlighted: trackLabel.modelData.locked
                        Accessible.name: qsTr("Track lock")
                        Accessible.checkable: true
                        Accessible.checked: trackLabel.modelData.locked
                        onClicked: root.session.setTrackLocked(trackLabel.modelData.id, !trackLabel.modelData.locked)
                    }
                }
                Label {
                    x: 18
                    y: 2
                    visible: trackLabel.modelData.collapsed || root.labelWidth < 120
                    anchors.right: parent.right
                    anchors.rightMargin: 30
                    horizontalAlignment: Text.AlignRight
                    color: ExoTheme.accent
                    text: (trackLabel.modelData.enabled ? "" : "M ") + (trackLabel.modelData.solo ? "S " : "") + (trackLabel.modelData.locked ? "L" : "")
                    Accessible.ignored: true
                }
                // The grip is the only reorder handle: the name selects the track
                // and the controls keep their meaning. A drag moves only the
                // insertion marker; release writes the order, Escape writes nothing.
                Item {
                    id: grip
                    objectName: "editTrackGrip" + trackLabel.modelData.id
                    x: 2
                    y: 2
                    width: 12
                    height: 22
                    enabled: !trackLabel.modelData.locked
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Reorder %1. Use Up and Down.").arg(trackLabel.modelData.name)
                    Accessible.description: enabled ? "" : qsTr("The track is locked.")
                    function move(step: int): void {
                        const destination = trackLabel.index + step;
                        const id = trackLabel.modelData.id;
                        if (destination >= 0 && destination < root.trackLayout.rows.length
                                && root.session.preflightPendingDraft() && root.session.reorderTrack(id, destination))
                            Qt.callLater(() => root.focusTrackGrip(id));
                    }
                    Keys.onUpPressed: event => { grip.move(-1); event.accepted = true; }
                    Keys.onDownPressed: event => { grip.move(1); event.accepted = true; }
                    Rectangle {
                        anchors.fill: parent
                        radius: 3
                        color: gripMouse.containsMouse || gripMouse.pressed ? ExoTheme.surfaceRaised : "transparent"
                        border.width: grip.activeFocus ? 1 : 0
                        border.color: ExoTheme.text
                    }
                    ExoGlyph {
                        anchors.centerIn: parent
                        width: 12
                        height: 18
                        kind: ExoGlyph.Grip
                        strokeWidth: 2
                        color: !grip.enabled ? ExoTheme.textDim : grip.activeFocus || gripMouse.pressed ? ExoTheme.accent : ExoTheme.textSecondary
                        Accessible.ignored: true
                    }
                    MouseArea {
                        id: gripMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: grip.enabled ? Qt.SizeVerCursor : Qt.ArrowCursor
                        preventStealing: true
                        property real pressY: 0
                        property bool started: false
                        function update(point: point): void {
                            const local = scroll.contentItem.mapFromItem(root, point.x, point.y);
                            const rows = root.trackLayout.rows;
                            const track = root.trackLayout.trackAtY(Math.max(0, local.y - root.rulerHeight));
                            root.reorderIndex = track ? rows.findIndex(row => row.id === track)
                                : local.y - root.rulerHeight < 0 ? 0 : rows.length - 1;
                        }
                        onPressed: mouse => {
                            if (!root.session.preflightPendingDraft()) { mouse.accepted = false; return; }
                            grip.forceActiveFocus(Qt.MouseFocusReason);
                            pressY = mapToItem(root, mouse.x, mouse.y).y;
                            started = false;
                            root.reorderTrackId = trackLabel.modelData.id;
                            root.reorderIndex = trackLabel.index;
                            root.pointerScrollVertical = true;
                            root.pointerPosition = mapToItem(root, mouse.x, mouse.y);
                            root.pointerUpdater = physical => gripMouse.update(physical);
                        }
                        onPositionChanged: mouse => {
                            if (!pressed || root.reorderTrackId !== trackLabel.modelData.id)
                                return;
                            const point = mapToItem(root, mouse.x, mouse.y);
                            if (!started && Math.abs(point.y - pressY) < Qt.styleHints.startDragDistance)
                                return;
                            started = true;
                            root.pointerPosition = point;
                            root.pointerScrolling = true;
                            update(point);
                        }
                        onReleased: {
                            root.stopPointerScroll();
                            const id = trackLabel.modelData.id;
                            const destination = root.reorderIndex;
                            const active = root.reorderTrackId === id;
                            root.reorderTrackId = 0;
                            if (active && started && destination >= 0 && destination !== trackLabel.index
                                    && root.session.reorderTrack(id, destination))
                                Qt.callLater(() => root.focusTrackGrip(id));
                        }
                        onCanceled: {
                            root.stopPointerScroll();
                            root.reorderTrackId = 0;
                        }
                        onDoubleClicked: root.trackLayout.toggleCollapsed(trackLabel.modelData.id)
                    }
                }
                MouseArea {
                    objectName: "editTrackResize" + trackLabel.modelData.id
                    anchors.bottom: parent.bottom
                    width: Math.max(0, parent.width - 6)
                    height: 4
                    cursorShape: Qt.SizeVerCursor
                    preventStealing: true
                    property real originalHeight: 0
                    property real pressY: 0
                    onPressed: mouse => {
                        if (!root.session.preflightPendingDraft()) { mouse.accepted = false; return; }
                        originalHeight = trackLabel.height;
                        pressY = mapToItem(root, mouse.x, mouse.y).y;
                    }
                    onPositionChanged: mouse => {
                        if (pressed)
                            root.trackLayout.setTrackHeight(trackLabel.modelData.id, originalHeight + mapToItem(root, mouse.x, mouse.y).y - pressY);
                    }
                }
                ExoMenu {
                    id: trackMenu
                    ExoMenuItem {
                        text: qsTr("Track settings")
                        onTriggered: {
                            root.trackEditorId = trackLabel.modelData.id;
                            trackEditor.open();
                        }
                    }
                    ExoMenuItem {
                        text: trackLabel.modelData.video ? qsTr("Toggle visibility") : qsTr("Toggle mute")
                        enabled: !trackLabel.modelData.locked
                        onTriggered: root.session.setTrackEnabled(trackLabel.modelData.id, !trackLabel.modelData.enabled)
                    }
                    ExoMenuItem {
                        text: qsTr("Toggle solo")
                        visible: !trackLabel.modelData.video
                        onTriggered: root.session.setTrackSolo(trackLabel.modelData.id, !trackLabel.modelData.solo)
                    }
                    ExoMenuItem {
                        text: qsTr("Toggle lock")
                        onTriggered: root.session.setTrackLocked(trackLabel.modelData.id, !trackLabel.modelData.locked)
                    }
                    ExoMenuItem {
                        text: trackLabel.modelData.collapsed ? qsTr("Expand track") : qsTr("Collapse track")
                        onTriggered: root.trackLayout.toggleCollapsed(trackLabel.modelData.id)
                    }
                    ExoMenuItem {
                        text: qsTr("Delete track")
                        onTriggered: {
                            root.trackToDelete = {id: trackLabel.modelData.id, name: trackLabel.modelData.name};
                            if (!root.session.deleteTrack(root.trackToDelete.id)) trackDeleteConfirmation.open();
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
            model: spacerModel
            delegate: Rectangle {
                id: spacerItem
                readonly property var trackBounds: root.trackGeometry(modelData.trackId, false)
                required property var modelData
                objectName: "editSpacer" + modelData.id
                readonly property bool donor: root.donorMaps.spacers[String(modelData.id)] ?? false
                opacity: donor ? 0.3 : 1
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
                    onPressed: mouse => { startX = mapToItem(scroll.contentItem, mouse.x, 0).x; started = false; root.capturedSpacerId = spacerItem.modelData.id; root.forceActiveFocus(); }
                    onPositionChanged: mouse => {
                        if (!pressed || root.capturedSpacerId !== spacerItem.modelData.id) return;
                        const delta = mapToItem(scroll.contentItem, mouse.x, 0).x - startX;
                        if (!started && Math.abs(delta) >= Qt.styleHints.startDragDistance) { started = true; root.session.beginSpacerDrag(spacerItem.modelData.id); }
                        if (started && root.session.dragActive) {
                            root.capturedSpacerId = spacerItem.modelData.id;
                            root.session.updateClipDragUs(Math.round(delta / root.pixelsPerUs));
                        }
                    }
                    onReleased: {
                        if (started) { if (root.session.dragActive) root.session.commitClipDrag(); }
                        else root.session.selectSpacer(spacerItem.modelData.id);
                        root.capturedSpacerId = 0;
                    }
                    onCanceled: { root.capturedSpacerId = 0; root.session.cancelClipDrag(); }
                }
                Repeater {
                    model: 2
                    delegate: MouseArea {
                        required property int index
                        property real startX: 0
                        property bool started: false
                        x: index === 0 ? 0 : spacerItem.width - width
                        visible: root.capturedSpacerId === spacerItem.modelData.id || ((index === 0 ? spacerItem.modelData.startUs >= viewport.startUs : spacerItem.modelData.endUs <= viewport.endUs) && spacerItem.width > 0)
                        width: 8; height: spacerItem.height
                        preventStealing: true
                        cursorShape: Qt.SizeHorCursor
                        onPressed: mouse => { startX = mapToItem(scroll.contentItem, mouse.x, 0).x; started = false; root.capturedSpacerId = spacerItem.modelData.id; root.forceActiveFocus(); }
                        onPositionChanged: mouse => {
                            if (!pressed || root.capturedSpacerId !== spacerItem.modelData.id) return;
                            const delta = mapToItem(scroll.contentItem, mouse.x, 0).x - startX;
                            if (!started && Math.abs(delta) >= Qt.styleHints.startDragDistance) { started = true; root.session.beginSpacerDrag(spacerItem.modelData.id, index); }
                            if (started && root.session.dragActive) {
                                root.capturedSpacerId = spacerItem.modelData.id;
                                root.session.updateClipDragUs(Math.round(delta / root.pixelsPerUs));
                            }
                        }
                        onReleased: {
                            if (started && root.session.dragActive) root.session.commitClipDrag();
                            root.capturedSpacerId = 0;
                        }
                        onCanceled: { root.capturedSpacerId = 0; root.session.cancelClipDrag(); }
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
                readonly property real authoredWidth: (modelData.endUs - modelData.startUs) * root.pixelsPerUs
                readonly property bool narrowTick: authoredWidth < 6
                readonly property real visibleLeft: Math.min(6, width / 2)
                readonly property real visibleWidth: Math.max(0, width - 2 * visibleLeft)
                readonly property real visibleSourceIn: Math.min(modelData.sourceOutUs, modelData.sourceInUs + Math.max(0, viewport.startUs - modelData.startUs) + visibleLeft / root.pixelsPerUs)
                readonly property real visibleSourceOut: Math.min(modelData.sourceOutUs, visibleSourceIn + visibleWidth / root.pixelsPerUs)
                readonly property bool bodyVisible: trackBounds.height >= 44 && !narrowTick
                readonly property real bodyHeight: Math.max(0, trackBounds.contentHeight - 2)
                readonly property bool previewVisible: bodyVisible && root.visible && y + clipItem.trackBounds.headerHeight + 1 + bodyHeight > scroll.contentY + root.rulerHeight && y + clipItem.trackBounds.headerHeight + 1 < scroll.contentY + scroll.height
                readonly property bool cachedVisual: previewVisible && modelData.available && modelData.sourceIdentity.length > 0 && root.thumbnails !== null
                property string waveformKey: ""
                property string mediaRequest: ""
                property string mediaRelevance: ""
                readonly property real playheadSourceUs: root.session.positionUs >= modelData.startUs && root.session.positionUs < modelData.endUs
                    ? modelData.sourceInUs + root.session.positionUs - modelData.startUs : -1
                readonly property var trimCandidate: root.capturedClipId === modelData.id && clipMouse.started && root.session.dragActive
                    ? root.ghostRows.find(row => row.id === modelData.id) : null
                readonly property real trimSourceUs: trimCandidate
                    ? clipMouse.pressTarget === EditInteractionController.PointerTrimStart ? trimCandidate.sourceInUs
                    : clipMouse.pressTarget === EditInteractionController.PointerTrimEnd ? trimCandidate.sourceOutUs - 1 : -1 : -1
                // A delegate outlives most publications, and modelData also changes for selection and
                // timeline position. Only a changed source or visible source window may replace the
                // tile and waveform requests. Tiles are keyed by source time, so a window that shrinks
                // or grows keeps the tile images it still shows instead of reloading every one.
                EditTimelineClipModel { id: tileModel }
                function refreshMedia(): void {
                    const shown = cachedVisual && visibleSourceOut > visibleSourceIn;
                    const video = shown && modelData.video;
                    const audio = shown && !modelData.video && modelData.streamIdentity.length > 0;
                    const request = video ? ["video", modelData.sourceIdentity, modelData.path, modelData.width, modelData.height, modelData.fpsNum, modelData.fpsDen, visibleSourceIn, visibleSourceOut, visibleWidth, bodyHeight, Screen.devicePixelRatio, root.thumbnails.sourceRevision].join("|")
                                          : audio ? ["audio", modelData.sourceIdentity, modelData.path, modelData.containerIndex, modelData.streamIdentity, modelData.sourceEndUs].join("|") : "";
                    const relevance = video ? [request, playheadSourceUs, trimSourceUs].join("|") : "";
                    if (request === mediaRequest) {
                        if (relevance !== mediaRelevance && video)
                            root.thumbnails.updateThumbnailRelevance(modelData.id, playheadSourceUs, trimSourceUs);
                        mediaRelevance = relevance;
                        return;
                    }
                    mediaRelevance = relevance;
                    mediaRequest = request;
                    if (video) {
                        tileModel.setRows(root.thumbnails.thumbnailTilesForClip(modelData.id, modelData.sourceIdentity, visibleSourceIn, visibleSourceOut, visibleWidth, bodyHeight, Screen.devicePixelRatio, playheadSourceUs, trimSourceUs));
                    } else {
                        if (root.thumbnails)
                            root.thumbnails.retireThumbnailInterest(modelData.id);
                        tileModel.setRows([]);
                    }
                    waveformKey = audio ? root.thumbnails.waveformSource(modelData.sourceIdentity, modelData.path, modelData.containerIndex, modelData.streamIdentity, modelData.sourceEndUs, modelData.id, Math.abs(modelData.startUs - root.session.positionUs)) : "";
                }
                readonly property string mediaSource: [modelData.video, modelData.sourceIdentity, modelData.path, modelData.containerIndex, modelData.streamIdentity].join("|")
                function retireMedia(): void {
                    if (root.thumbnails)
                        root.thumbnails.retireThumbnailInterest(modelData.id);
                    mediaRequest = "";
                    mediaRelevance = "";
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
                onBodyHeightChanged: requestMediaRefresh()
                onPlayheadSourceUsChanged: root.scheduleVisualWork()
                onTrimSourceUsChanged: root.scheduleVisualWork()
                Component.onCompleted: requestMediaRefresh()
                Component.onDestruction: if (root.thumbnails)
                    root.thumbnails.retireThumbnailInterest(modelData.id)
                x: root.visibleX(modelData.startUs)
                y: root.rulerHeight + trackBounds.top
                width: narrowTick && modelData.endUs > viewport.startUs && modelData.startUs < viewport.endUs
                    ? Math.max(2, root.visibleSpan(modelData.startUs, modelData.endUs)) : root.visibleSpan(modelData.startUs, modelData.endUs)
                height: trackBounds.height
                readonly property bool donor: root.donorMap[String(modelData.id)] ?? false
                readonly property bool linkedHover: root.session.linkedSelection && modelData.group !== 0 && root.hoveredGroup === modelData.group
                readonly property bool disabled: modelData.enabled === false || trackBounds.enabled === false
                readonly property real contentOpacity: donor ? 0.3 : disabled ? 0.55 : 1
                readonly property color trackColor: trackBounds.color || ExoTheme.accent
                readonly property color trackTint: Qt.hsla(Math.max(0, trackColor.hslHue), Math.min(0.25, trackColor.hslSaturation), ExoTheme.dark ? Math.min(0.65, trackColor.hslLightness) : Math.max(0.35, trackColor.hslLightness), 1)
                color: narrowTick ? trackColor : Qt.tint(ExoTheme.surfaceRaised, Qt.rgba(trackTint.r, trackTint.g, trackTint.b, disabled || donor ? 0.07 : 0.14))
                border.color: ExoTheme.line
                border.width: narrowTick ? 0 : 1
                clip: !narrowTick
                radius: narrowTick || modelData.fadeInOwned || modelData.fadeOutOwned ? 0 : 4
                Rectangle {
                    objectName: "editClipHeader" + clipItem.modelData.id
                    x: 1
                    y: 1
                    width: Math.max(0, parent.width - 2)
                    height: Math.max(0, clipItem.trackBounds.headerHeight - 1)
                    color: Qt.tint(ExoTheme.surfaceRaised, Qt.rgba(clipItem.trackTint.r, clipItem.trackTint.g, clipItem.trackTint.b, clipItem.modelData.selected ? 0.4 : clipItem.linkedHover ? 0.3 : 0.22))
                    Accessible.ignored: true
                }
                Rectangle {
                    x: 1
                    y: 0
                    width: Math.max(0, parent.width - 2)
                    height: 1
                    visible: clipItem.modelData.selected
                    color: ExoTheme.accent
                    Accessible.ignored: true
                }
                Canvas {
                    x: 1
                    y: clipItem.trackBounds.headerHeight
                    width: Math.max(0, parent.width - 2)
                    height: Math.max(0, parent.height - y - 1)
                    visible: !clipItem.modelData.available && clipItem.bodyVisible
                    opacity: 0.25
                    property color strokeColor: ExoTheme.textMuted
                    onStrokeColorChanged: requestPaint()
                    onVisibleChanged: if (visible) requestPaint()
                    onWidthChanged: if (visible) requestPaint()
                    onHeightChanged: if (visible) requestPaint()
                    onPaint: {
                        const ctx = getContext("2d");
                        ctx.clearRect(0, 0, width, height);
                        ctx.strokeStyle = strokeColor;
                        ctx.lineWidth = 1;
                        ctx.beginPath();
                        for (let offset = -height; offset < width; offset += 10) {
                            ctx.moveTo(offset, height);
                            ctx.lineTo(offset + height, 0);
                        }
                        ctx.stroke();
                    }
                    Accessible.ignored: true
                }
                Canvas {
                    anchors.fill: parent
                    visible: root.donorMap[String(clipItem.modelData.id)] ?? false
                    property color strokeColor: ExoTheme.accent
                    onStrokeColorChanged: requestPaint()
                    onVisibleChanged: if (visible) requestPaint()
                    onWidthChanged: if (visible) requestPaint()
                    onHeightChanged: if (visible) requestPaint()
                    onPaint: {
                        const ctx = getContext("2d");
                        ctx.clearRect(0, 0, width, height);
                        ctx.strokeStyle = strokeColor;
                        ctx.lineWidth = 1;
                        ctx.setLineDash([4, 3]);
                        ctx.strokeRect(0.5, 0.5, Math.max(0, width - 1), Math.max(0, height - 1));
                    }
                    z: 5
                    Accessible.ignored: true
                }
                Rectangle {
                    anchors.right: parent.right
                    width: 4
                    height: parent.height
                    visible: root.draggingCrossfade && clipItem.modelData.video
                    color: root.session.canCrossfadeAtCut(clipItem.modelData.id) ? ExoTheme.accent : ExoTheme.warning
                    z: 10
                    Accessible.ignored: true
                }
                Accessible.role: Accessible.Button
                Accessible.name: modelData.name + (modelData.available ? "" : ". " + qsTr("Media unavailable")) + (disabled ? ". " + qsTr("Disabled") : "")
                Accessible.onPressAction: root.session.selectClip(modelData.id)
                Image {
                    id: poster
                    objectName: "editClipPoster" + clipItem.modelData.id
                    x: clipItem.visibleLeft
                    y: clipItem.trackBounds.headerHeight + 1
                    width: 64
                    height: clipItem.bodyHeight
                    source: clipItem.previewVisible && !clipItem.cachedVisual && clipItem.modelData.available && clipItem.modelData.video && root.thumbnails
                        ? root.thumbnails.posterForIdentity(clipItem.modelData.sourceIdentity) : ""
                    sourceSize: Qt.size(64, 44)
                    visible: status === Image.Ready && clipItem.modelData.available && clipItem.previewVisible
                    opacity: clipItem.contentOpacity
                    fillMode: Image.PreserveAspectFit
                    Accessible.ignored: true
                }
                Repeater {
                    model: tileModel
                    delegate: Image {
                        objectName: "editClipThumbnail"
                        required property var modelData
                        x: clipItem.visibleLeft + modelData.x
                        y: clipItem.trackBounds.headerHeight + 1
                        width: modelData.width
                        height: clipItem.bodyHeight
                        source: modelData.source
                        sourceSize: Qt.size(width, 44)
                        asynchronous: true
                        cache: false
                        visible: status === Image.Ready && clipItem.previewVisible
                        opacity: clipItem.contentOpacity
                        fillMode: Image.PreserveAspectCrop
                        Accessible.ignored: true
                    }
                }
                ExoWaveformItem {
                    objectName: "editClipWaveform" + clipItem.modelData.id
                    x: clipItem.visibleLeft
                    y: clipItem.trackBounds.headerHeight + 1
                    width: clipItem.visibleWidth
                    height: clipItem.bodyHeight
                    visible: clipItem.waveformKey.length > 0 && clipItem.previewVisible
                    opacity: clipItem.contentOpacity
                    service: root.thumbnails ? root.thumbnails.waveformService : null
                    cacheKey: clipItem.waveformKey
                    sourceInUs: clipItem.visibleSourceIn
                    sourceOutUs: clipItem.visibleSourceOut
                    channelCount: clipItem.modelData.sourceChannels === 1 ? 1 : 2
                    gainDb: clipItem.modelData.gainDb
                    color: ExoTheme.textMuted
                    Accessible.ignored: true
                }
                ExoGlyph {
                    id: mediaIcon
                    visible: !clipItem.narrowTick
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
                    objectName: "editClipName" + clipItem.modelData.id
                    visible: !clipItem.narrowTick
                    x: clipItem.visibleLeft + 22
                    y: 0
                    height: clipItem.trackBounds.headerHeight
                    verticalAlignment: Text.AlignVCenter
                    width: Math.max(0, clipItem.width - x - 10)
                    text: clipItem.modelData.name
                    color: ExoTheme.text
                    font.pixelSize: ExoTheme.fontCaption
                    elide: Text.ElideRight
                }
                MouseArea {
                    id: clipMouse
                    objectName: "editClipPointer" + clipItem.modelData.id
                    preventStealing: true
                    anchors.fill: parent
                    anchors.leftMargin: clipItem.narrowTick ? -4 : 0
                    anchors.rightMargin: clipItem.narrowTick ? -4 : 0
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    property point pressPoint: Qt.point(0, 0)
                    property real pressTimeUs: 0
                    property var pressClipId: 0
                    property var pressJunction: 0
                    property int pressTarget: EditInteractionController.PointerNone
                    property var pressPolicy: ({valid: false})
                    property bool started: false
                    property bool rangeIntent: false
                    property bool clickSeek: false
                    readonly property real authoredWidth: (clipItem.modelData.endUs - clipItem.modelData.startUs) * root.pixelsPerUs
                    onEntered: root.hoveredGroup = clipItem.modelData.group
                    onExited: if (root.hoveredGroup === clipItem.modelData.group) root.hoveredGroup = 0
                    onPressed: mouse => {
                        if (!root.session.preflightPendingDraft()) {
                            mouse.accepted = false;
                            return;
                        }
                        const point = mapToItem(scroll.contentItem, mouse.x, mouse.y);
                        pressPoint = mapToItem(root, mouse.x, mouse.y);
                        pressTimeUs = root.timeAtUs(point.x);
                        pressClipId = clipItem.narrowTick
                            ? root.interaction.resolveNarrowClip(clipModel, clipItem.modelData.trackId, pressTimeUs, root.pixelsPerUs)
                            : clipItem.modelData.id;
                        if (!clipItem.narrowTick)
                            root.interaction.resolveNarrowClip(null, 0, 0, 0);
                        if (!pressClipId) {
                            mouse.accepted = false;
                            return;
                        }
                        if (mouse.button === Qt.RightButton) {
                            // Keep a multi-selection the menu is about to act on.
                            if (!root.session.selectedClipIds.some(id => id === pressClipId))
                                root.session.selectClip(pressClipId);
                            root.forceActiveFocus();
                            clipMenu.popup();
                            return;
                        }
                        started = false;
                        const authoredX = point.x - root.positionAtUs(clipItem.modelData.startUs);
                        pressJunction = root.interaction.modelJunctionAt(junctionModel, pressClipId, pressTimeUs);
                        pressTarget = clipItem.narrowTick ? EditInteractionController.PointerHeader :
                            root.interaction.clipPointerTarget(clipItem.modelData, authoredWidth,
                                clipItem.height, clipItem.trackBounds.headerHeight, authoredX, mouse.y, pressJunction !== 0);
                        const hasRange = root.session.selectionMode === EditSessionAdapter.RangeSelection;
                        // With the selection tool the whole clip body behaves like
                        // its header: click selects, drag moves the selection. A
                        // plain click still positions the playhead, on release, so
                        // a move never drags the playhead along.
                        clickSeek = root.tool === "select" && pressTarget === EditInteractionController.PointerContent
                            && mouse.modifiers === Qt.NoModifier;
                        if (root.tool === "select" && pressTarget === EditInteractionController.PointerContent)
                            pressTarget = EditInteractionController.PointerHeader;
                        pressPolicy = root.interaction.clipPointerPolicy(pressTarget, mouse.modifiers, hasRange);
                        root.inputHint = pressPolicy.error;
                        if (!pressPolicy.valid) {
                            root.forceActiveFocus();
                            return;
                        }
                        rangeIntent = pressPolicy.range;
                        if (rangeIntent) {
                            const anchor = pressPolicy.anchorAtPlayhead ? root.session.positionUs : pressTimeUs;
                            if (!root.session.beginRangeGestureUs(anchor, [clipItem.modelData.trackId], pressPolicy.extend, pressPolicy.preserveBounds)) {
                                mouse.accepted = false;
                                return;
                            }
                            root.session.updateRangeGestureToTrackUs(pressTimeUs, clipItem.modelData.trackId);
                        } else if (pressTarget === EditInteractionController.PointerJunction) {
                            root.session.selectTransition(pressJunction);
                        } else {
                            if (!root.session.beginClipPointerSelection(pressClipId, pressPolicy.toggle, pressPolicy.extend, pressPolicy.preserveSelection)) {
                                mouse.accepted = false;
                                return;
                            }
                            if (pressPolicy.seek)
                                root.session.requestSeekUs(pressTimeUs);
                        }
                        root.capturedClipId = clipItem.modelData.id;
                        root.dragTrack = clipItem.modelData.trackId;
                        root.pointerPosition = pressPoint;
                        root.pointerScrollVertical = true;
                        root.pointerUpdater = physical => clipMouse.updatePointer(physical);
                        root.forceActiveFocus();
                    }
                    function updatePointer(physical: point): void {
                        if (!pressed || !(pressedButtons & Qt.LeftButton) || !pressPolicy.valid ||
                                (!root.session.hasPointerSelection() && !root.session.dragActive && !root.session.rangeGestureActive))
                            return;
                        const point = scroll.contentItem.mapFromItem(root, physical.x, physical.y);
                        const dx = physical.x - pressPoint.x;
                        const dy = physical.y - pressPoint.y;
                        if (!started && root.interaction.clipPointerThreshold(pressTarget, dx, dy, Qt.styleHints.startDragDistance)) {
                            started = true;
                            if (pressTarget === EditInteractionController.PointerContent) {
                                if (!rangeIntent)
                                    root.session.beginRangeGestureUs(pressTimeUs, [clipItem.modelData.trackId]);
                            } else if (pressTarget === EditInteractionController.PointerGain) {
                                root.session.beginClipGainDrag(clipItem.modelData.id, pressPolicy.fine);
                            } else {
                                const edge = pressTarget === EditInteractionController.PointerTrimStart ? 0 :
                                    pressTarget === EditInteractionController.PointerTrimEnd ? 1 :
                                    pressTarget === EditInteractionController.PointerFadeIn ? 2 :
                                    pressTarget === EditInteractionController.PointerFadeOut ? 3 : -1;
                                root.session.beginClipDrag(pressClipId, edge, pressPolicy.unlink);
                            }
                        }
                        if (!started)
                            return;
                        root.pointerScrolling = pressTarget !== EditInteractionController.PointerGain;
                        if (pressTarget === EditInteractionController.PointerContent) {
                            const track = root.trackLayout.trackAtY(point.y - root.rulerHeight);
                            root.session.updateRangeGestureToTrackUs(root.timeAtUs(point.x), track || clipItem.modelData.trackId);
                        } else if (pressTarget === EditInteractionController.PointerGain) {
                            root.session.updateClipGainDrag(dy);
                        } else if (pressTarget === EditInteractionController.PointerFadeIn || pressTarget === EditInteractionController.PointerFadeOut) {
                            root.session.updateClipFadeDragPixels(dx, root.pixelsPerUs, pressPolicy.fine);
                        } else if (root.session.dragActive) {
                            const delta = root.timeAtUs(point.x) - pressTimeUs;
                            if (pressTarget === EditInteractionController.PointerHeader)
                                root.previewDrag(delta, point.y, pressPolicy.insert);
                            else
                                root.session.updateClipDragUs(Math.round(delta), 0, -1, false, Math.round(6 / root.pixelsPerUs));
                        }
                    }
                    onPositionChanged: mouse => {
                        root.pointerPosition = mapToItem(root, mouse.x, mouse.y);
                        updatePointer(root.pointerPosition);
                    }
                    onReleased: mouse => {
                        root.stopPointerScroll();
                        root.capturedClipId = 0;
                        if (mouse.button === Qt.RightButton || !pressPolicy.valid)
                            return;
                        if (root.session.rangeGestureActive) {
                            if (started || (rangeIntent && pressPolicy.extend))
                                root.session.commitRangeGesture();
                            else
                                root.session.cancelRangeGesture();
                        } else if (started && root.session.dragActive) {
                            root.session.commitClipDrag();
                        }
                        root.session.finishPointerSelection(false, started);
                        if (pressPolicy.seek)
                            root.session.finishPointerSeek();
                        else if (clickSeek && !started) {
                            root.session.requestSeekUs(pressTimeUs);
                            root.session.finishPointerSeek();
                        }
                    }
                    onCanceled: {
                        root.stopPointerScroll();
                        root.capturedClipId = 0;
                        root.session.cancelClipDrag();
                        root.session.cancelRangeGesture();
                        root.session.finishPointerSelection(true);
                    }
                    onDoubleClicked: mouse => {
                        if (pressTarget === EditInteractionController.PointerContent) {
                            root.session.setEditRangeUs(clipItem.modelData.startUs, clipItem.modelData.endUs, [clipItem.modelData.trackId]);
                        } else if (pressTarget === EditInteractionController.PointerGain) {
                            root.session.setClipGain(clipItem.modelData.id, 0);
                        } else if (pressTarget === EditInteractionController.PointerFadeIn || pressTarget === EditInteractionController.PointerFadeOut) {
                            root.session.applyTimeValue(clipItem.modelData.id,
                                pressTarget === EditInteractionController.PointerFadeIn ? "fadeIn" : "fadeOut", 0);
                        } else if (root.session.inspectClip(pressClipId)) {
                            const field = pressTarget === EditInteractionController.PointerTrimStart ? "sourceIn" :
                                pressTarget === EditInteractionController.PointerTrimEnd ? "sourceOut" : "";
                            root.inspectorOpenRequested(field);
                        }
                    }
                }
                ToolTip.visible: clipMouse.containsMouse
                ToolTip.text: clipItem.Accessible.name + "\n" + clipItem.modelData.path
                Rectangle {
                    objectName: "editClipGain" + clipItem.modelData.id
                    visible: !clipItem.narrowTick && !clipItem.modelData.video && clipItem.bodyHeight >= 26
                    y: root.interaction.clipGainLineY(clipItem.modelData, clipItem.height, clipItem.trackBounds.headerHeight)
                    width: clipItem.width
                    height: 1
                    color: ExoTheme.accent
                    Accessible.ignored: true
                }
                Repeater {
                    model: 2
                    delegate: Canvas {
                        id: fadeEnvelope
                        required property int index
                        readonly property real durationUs: index === 0 ? clipItem.modelData.fadeInUs : clipItem.modelData.fadeOutUs
                        readonly property real fullWidth: durationUs * root.pixelsPerUs
                        readonly property real rawX: root.positionAtUs(index === 0 ? clipItem.modelData.startUs : clipItem.modelData.endUs - durationUs) - clipItem.x
                        readonly property real fromProgress: fullWidth > 0 ? Math.max(0, -rawX) / fullWidth : 0
                        readonly property real toProgress: fullWidth > 0 ? Math.min(fullWidth, clipItem.width - rawX) / fullWidth : 1
                        readonly property var samples: visible ? root.interaction.fadeEnvelopePoints(
                            index === 0 ? clipItem.modelData.fadeInCurve ?? 0 : clipItem.modelData.fadeOutCurve ?? 0,
                            index === 0 ? clipItem.modelData.fadeInBias ?? 0 : clipItem.modelData.fadeOutBias ?? 0,
                            index === 1, fromProgress, toProgress) : []
                        objectName: "editFadeEnvelope" + clipItem.modelData.id + "_" + index
                        x: Math.max(0, rawX)
                        y: clipItem.trackBounds.headerHeight
                        width: Math.max(0, Math.min(clipItem.width, rawX + fullWidth) - x)
                        height: clipItem.bodyHeight
                        visible: durationUs > 0 && width > 0 && height > 0 && !(index === 0 ? clipItem.modelData.fadeInOwned : clipItem.modelData.fadeOutOwned)
                        onSamplesChanged: requestPaint()
                        onWidthChanged: requestPaint()
                        onHeightChanged: requestPaint()
                        onPaint: {
                            const ctx = getContext("2d");
                            ctx.clearRect(0, 0, width, height);
                            if (samples.length < 2)
                                return;
                            ctx.beginPath();
                            ctx.moveTo(0, 0);
                            for (let point = 0; point < samples.length; ++point)
                                ctx.lineTo(width * point / (samples.length - 1), height * (1 - samples[point]));
                            ctx.lineTo(width, 0);
                            ctx.closePath();
                            ctx.fillStyle = Qt.rgba(ExoTheme.accent.r, ExoTheme.accent.g, ExoTheme.accent.b, 0.12);
                            ctx.fill();
                            ctx.beginPath();
                            for (let point = 0; point < samples.length; ++point) {
                                const x = width * point / (samples.length - 1);
                                const y = height * (1 - samples[point]);
                                if (point === 0) ctx.moveTo(x, y);
                                else ctx.lineTo(x, y);
                            }
                            ctx.strokeStyle = ExoTheme.accent;
                            ctx.lineWidth = 1;
                            ctx.stroke();
                        }
                        Accessible.ignored: true
                    }
                }
                Repeater {
                    model: 2
                    delegate: Rectangle {
                        required property int index
                        objectName: "editFadeHandle" + clipItem.modelData.id + "_" + index
                        readonly property real edgeX: root.positionAtUs(index === 0 ? clipItem.modelData.startUs : clipItem.modelData.endUs) - clipItem.x
                        x: index === 0 ? edgeX : edgeX - width
                        y: 0
                        width: 8
                        height: 8
                        radius: 2
                        color: ExoTheme.accent
                        visible: x >= 0 && x + width <= clipItem.width &&
                            root.interaction.clipPointerTarget(clipItem.modelData, clipMouse.authoredWidth,
                                clipItem.height, clipItem.trackBounds.headerHeight,
                                index === 0 ? 0 : clipMouse.authoredWidth - 1, 0) ===
                            (index === 0 ? EditInteractionController.PointerFadeIn : EditInteractionController.PointerFadeOut)
                        Accessible.ignored: true
                    }
                }
            }
        }
        Label {
            objectName: "editNarrowClipName"
            readonly property var row: root.session.selectedClipDetails
            readonly property var bounds: root.trackGeometry(row.trackId ?? 0, false)
            visible: !!row.id && (row.endUs - row.startUs) * root.pixelsPerUs < 6 &&
                row.endUs > viewport.startUs && row.startUs < viewport.endUs &&
                bounds.top + bounds.height > scroll.contentY && bounds.top < scroll.contentY + scroll.height - root.rulerHeight
            x: Math.max(scroll.contentX + root.labelWidth, Math.min(scroll.contentX + scroll.width - width - 14,
                root.visibleX(row.startUs ?? 0) + 4))
            y: root.rulerHeight + (bounds.top ?? 0) + (bounds.height >= 44 ? bounds.headerHeight : 0)
            width: Math.min(180, Math.max(0, viewport.usableWidth - 8))
            text: row.name ?? ""
            elide: Text.ElideRight
            color: ExoTheme.text
            font.pixelSize: ExoTheme.fontCaption
            background: Rectangle { color: ExoTheme.surfaceRaised; border.color: ExoTheme.line; radius: 2 }
            z: 9
            Accessible.ignored: true
        }
        Repeater {
            model: candidateClipModel
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
                color: ExoTheme.surfaceHover
                opacity: 0.7
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
            model: candidateSpacerModel
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
            model: candidateJunctionModel
            delegate: Rectangle {
                id: junctionGhost
                required property var modelData
                objectName: "editDragJunctionGhost"
                readonly property var trackBounds: root.trackGeometry(modelData.trackId, true)
                readonly property bool hardCut: modelData.kind === "hardCut"
                x: hardCut ? root.visibleX(modelData.cutUs) : Math.max(scroll.contentX + root.labelWidth, root.visibleX(modelData.startUs))
                y: root.rulerHeight + trackBounds.top + trackBounds.headerHeight
                width: hardCut ? 1 : Math.max(0, Math.min(scroll.contentX + scroll.width, root.visibleX(modelData.endUs)) - x)
                height: Math.max(0, trackBounds.height - trackBounds.headerHeight)
                z: 8
                color: hardCut ? border.color : "transparent"
                border.width: hardCut ? 0 : 1
                border.color: root.session.dragValid ? ExoTheme.accent : ExoTheme.warning
                opacity: 0.7
                Accessible.ignored: true
                Rectangle {
                    objectName: "editDragHardCut" + junctionGhost.modelData.id
                    x: -2
                    y: 1
                    width: 5
                    height: 5
                    rotation: 45
                    visible: junctionGhost.hardCut
                    color: junctionGhost.border.color
                    Accessible.ignored: true
                }
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
            model: junctionModel
            delegate: Item {
                id: transitionOverlay
                readonly property var trackBounds: root.trackGeometry(modelData.trackId, false)
                required property var modelData
                objectName: "editCrossfadeOverlay"
                readonly property bool hardCut: modelData.kind === "hardCut"
                readonly property bool donor: (root.donorMaps.junctions[String(modelData.id)] ?? false) || (root.donorMap[String(modelData.outgoing)] ?? false) || (root.donorMap[String(modelData.incoming)] ?? false)
                readonly property real fullWidth: (modelData.overlapEndUs - modelData.overlapStartUs) * root.pixelsPerUs
                readonly property real contentOffset: Math.max(0, viewport.startUs - modelData.overlapStartUs) * root.pixelsPerUs
                readonly property real cutX: root.positionAtUs(modelData.cutUs) - x
                x: root.visibleX(modelData.overlapStartUs)
                y: root.rulerHeight + trackBounds.top + trackBounds.headerHeight
                width: root.visibleSpan(modelData.overlapStartUs, modelData.overlapEndUs)
                height: Math.max(0, trackBounds.height - trackBounds.headerHeight)
                visible: height > 0
                opacity: transitionOverlay.donor ? 0.3 : 1
                z: 3
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: hardCut ? qsTr("Hard cut. Enter to adjust.") : qsTr("Crossfade, %1 ms. Enter to adjust duration.").arg(modelData.durationMs)
                function adjust(): void {
                    root.openTransitionPopup(modelData);
                }
                Keys.onReturnPressed: adjust()
                Keys.onDeletePressed: {
                    root.session.selectTransition(modelData.outgoing);
                    root.session.removeTransition();
                }
                Canvas {
                    id: junctionCurves
                    objectName: "editJunctionCurves" + transitionOverlay.modelData.id
                    anchors.fill: parent
                    visible: !transitionOverlay.hardCut && width > 0 && height > 0
                    clip: true
                    antialiasing: true
                    readonly property var envelope: transitionOverlay.modelData.envelope || ({})
                    readonly property color outgoingColor: ExoTheme.textSecondary
                    readonly property color incomingColor: ExoTheme.accent
                    onEnvelopeChanged: requestPaint()
                    onOutgoingColorChanged: requestPaint()
                    onIncomingColorChanged: requestPaint()
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    Connections {
                        target: transitionOverlay
                        function onContentOffsetChanged(): void { junctionCurves.requestPaint(); }
                        function onFullWidthChanged(): void { junctionCurves.requestPaint(); }
                        function onModelDataChanged(): void { junctionCurves.requestPaint(); }
                    }
                    onPaint: {
                        const ctx = getContext("2d");
                        ctx.clearRect(0, 0, width, height);
                        ctx.lineWidth = transitionOverlay.modelData.selected ? 2 : 1.25;
                        const curves = [envelope.outgoing || [], envelope.incoming || []];
                        for (let curve = 0; curve < curves.length; ++curve) {
                            const points = curves[curve];
                            if (points.length < 2)
                                continue;
                            ctx.strokeStyle = curve === 0 ? outgoingColor : incomingColor;
                            ctx.beginPath();
                            for (let point = 0; point < points.length; ++point) {
                                const x = point / (points.length - 1) * transitionOverlay.fullWidth - transitionOverlay.contentOffset;
                                const y = (1 - points[point]) * Math.max(0, height - 1);
                                if (point === 0) ctx.moveTo(x, y);
                                else ctx.lineTo(x, y);
                            }
                            ctx.stroke();
                        }
                    }
                    Accessible.ignored: true
                }
                Rectangle {
                    objectName: "editJunctionWindow" + transitionOverlay.modelData.id
                    x: root.visibleX(transitionOverlay.modelData.startUs) - transitionOverlay.x
                    width: root.visibleSpan(transitionOverlay.modelData.startUs, transitionOverlay.modelData.endUs)
                    height: parent.height
                    visible: !transitionOverlay.hardCut
                    color: "transparent"
                    border.color: transitionOverlay.modelData.selected ? ExoTheme.accent : ExoTheme.lineStrong
                    Accessible.ignored: true
                }
                Rectangle {
                    objectName: "editTinyTransitionMarker" + transitionOverlay.modelData.id
                    x: transitionOverlay.fullWidth / 2 - transitionOverlay.contentOffset - 1
                    width: 2
                    height: parent.height
                    visible: !transitionOverlay.hardCut && transitionOverlay.fullWidth < 8
                    color: transitionOverlay.modelData.selected ? ExoTheme.accent : ExoTheme.textSecondary
                    Accessible.ignored: true
                }
                Rectangle {
                    id: hardCutLine
                    objectName: "editHardCutLine" + transitionOverlay.modelData.id
                    x: transitionOverlay.cutX - 0.5
                    width: 1
                    height: parent.height
                    visible: transitionOverlay.hardCut
                    color: ExoTheme.accent
                    Accessible.ignored: true
                }
                Rectangle {
                    anchors.horizontalCenter: hardCutLine.horizontalCenter
                    y: 1
                    width: 5
                    height: 5
                    rotation: 45
                    visible: transitionOverlay.hardCut
                    color: ExoTheme.accent
                    Accessible.ignored: true
                }
                Label {
                    objectName: "editJunctionLabel" + transitionOverlay.modelData.id
                    x: Math.max(0, Math.min(transitionOverlay.width - width, transitionOverlay.hardCut ? transitionOverlay.cutX + 6 : 4))
                    y: -transitionOverlay.trackBounds.headerHeight
                    height: transitionOverlay.trackBounds.headerHeight
                    text: transitionOverlay.hardCut ? qsTr("Hard cut") : qsTr("%1 ms").arg(transitionOverlay.modelData.durationUs / 1000)
                    visible: transitionOverlay.hardCut || transitionMouse.containsMouse || transitionOverlay.activeFocus || transitionOverlay.modelData.selected
                    color: ExoTheme.text
                    font.family: ExoTheme.sansFamily
                    font.pixelSize: ExoTheme.fontCaption
                    background: Rectangle { color: ExoTheme.surfaceRaised; radius: 2 }
                    Accessible.ignored: true
                }
                MouseArea {
                    id: transitionMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    preventStealing: true
                    property point pressPoint: Qt.point(0, 0)
                    property real pressTimeUs: 0
                    property int capturedEdge: -1
                    property int lastEdge: -1
                    property bool started: false
                    property bool captured: false
                    readonly property real windowStartX: root.positionAtUs(transitionOverlay.modelData.startUs) - transitionOverlay.x
                    readonly property real windowEndX: root.positionAtUs(transitionOverlay.modelData.endUs) - transitionOverlay.x
                    readonly property bool shorterWindow: transitionOverlay.modelData.startUs > transitionOverlay.modelData.overlapStartUs || transitionOverlay.modelData.endUs < transitionOverlay.modelData.overlapEndUs
                    onPressed: mouse => {
                        const point = mapToItem(scroll.contentItem, mouse.x, mouse.y);
                        const at = root.timeAtUs(point.x);
                        if (!root.session.preflightPendingDraft() ||
                                !root.interaction.junctionPointerEligible(transitionOverlay.modelData, root.pixelsPerUs, at,
                                    mouse.y + transitionOverlay.trackBounds.headerHeight, transitionOverlay.trackBounds.height,
                                    transitionOverlay.trackBounds.headerHeight)) {
                            mouse.accepted = false;
                            return;
                        }
                        const policy = root.interaction.clipPointerPolicy(EditInteractionController.PointerJunction, mouse.modifiers, false);
                        if (!policy.valid) {
                            root.inputHint = policy.error;
                            return;
                        }
                        const selected = transitionOverlay.modelData.selected;
                        capturedEdge = transitionOverlay.hardCut && Math.abs(mouse.x - transitionOverlay.cutX) <= 4 ? 2 :
                            !transitionOverlay.hardCut && selected && shorterWindow
                                ? root.interaction.junctionHandleAt(mouse.x, windowStartX, windowEndX, lastEdge) : -1;
                        if (!root.session.beginClipPointerSelection(transitionOverlay.modelData.outgoing, false, false, true)) {
                            mouse.accepted = false;
                            return;
                        }
                        root.session.selectTransition(transitionOverlay.modelData.outgoing);
                        if (capturedEdge === 0 || capturedEdge === 1)
                            lastEdge = capturedEdge;
                        pressPoint = mapToItem(root, mouse.x, mouse.y);
                        pressTimeUs = at;
                        started = false;
                        captured = true;
                        root.capturedJunctionId = transitionOverlay.modelData.id;
                        root.pointerPosition = pressPoint;
                        root.pointerScrollVertical = false;
                        root.pointerUpdater = physical => transitionMouse.updatePointer(physical);
                        transitionOverlay.forceActiveFocus();
                    }
                    function updatePointer(physical: point): void {
                        if (!pressed || !captured || capturedEdge < 0 ||
                                (!root.session.hasPointerSelection() && !root.session.dragActive))
                            return;
                        const dx = physical.x - pressPoint.x;
                        if (!started && Math.abs(dx) >= Qt.styleHints.startDragDistance) {
                            root.session.beginJunctionDrag(transitionOverlay.modelData.outgoing, capturedEdge);
                            started = root.session.dragActive;
                        }
                        if (!started)
                            return;
                        const point = scroll.contentItem.mapFromItem(root, physical.x, physical.y);
                        root.session.updateClipDragUs(Math.round(root.timeAtUs(point.x) - pressTimeUs));
                        root.pointerScrolling = true;
                    }
                    onPositionChanged: mouse => {
                        root.pointerPosition = mapToItem(root, mouse.x, mouse.y);
                        updatePointer(root.pointerPosition);
                    }
                    onReleased: {
                        if (!captured)
                            return;
                        captured = false;
                        root.capturedJunctionId = 0;
            root.capturedSpacerId = 0;
                        root.stopPointerScroll();
                        if (started && root.session.dragActive)
                            root.session.commitClipDrag();
                        root.session.finishPointerSelection(false, started);
                    }
                    onCanceled: {
                        if (!captured)
                            return;
                        captured = false;
                        root.capturedJunctionId = 0;
            root.capturedSpacerId = 0;
                        root.stopPointerScroll();
                        root.session.cancelClipDrag();
                        root.session.finishPointerSelection(true);
                    }
                    onDoubleClicked: transitionOverlay.adjust()
                }
                Repeater {
                    model: 2
                    delegate: Rectangle {
                        required property int index
                        objectName: "editJunctionHandle" + transitionOverlay.modelData.id + "_" + index
                        visible: transitionOverlay.modelData.selected && transitionMouse.shorterWindow && !transitionOverlay.hardCut && (index === 0 ? transitionOverlay.modelData.startUs >= viewport.startUs : transitionOverlay.modelData.endUs <= viewport.endUs) && transitionOverlay.width > 0
                        x: root.positionAtUs(index === 0 ? transitionOverlay.modelData.startUs : transitionOverlay.modelData.endUs) - transitionOverlay.x - width / 2
                        width: 1
                        height: transitionOverlay.height
                        color: ExoTheme.accent
                        Accessible.ignored: true
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
            height: root.rangeBandTop
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
            height: root.rangeBandTop
            onPressed: mouse => {
                root.forceActiveFocus();
                root.player.beginScrub();
                root.session.requestSeekUs(root.timeAtUs(mouse.x + root.labelWidth));
            }
            onPositionChanged: mouse => {
                if (pressed)
                    root.session.requestSeekUs(root.timeAtUs(mouse.x + root.labelWidth));
            }
            onReleased: {
                root.session.finishPointerSeek();
                root.player.endScrub();
            }
            onCanceled: {
                root.session.finishPointerSeek();
                root.player.endScrub();
            }
            onDoubleClicked: root.fitTimeline()
        }
        Rectangle {
            objectName: "editRangeBandBackground"
            x: root.labelWidth
            y: scroll.contentY + root.rangeBandTop
            width: Math.max(0, scroll.width - root.labelWidth)
            height: root.rangeBandHeight
            color: ExoTheme.surfaceRaised
            z: 5
            Rectangle {
                readonly property var range: root.displayedRange
                visible: range.startUs !== undefined
                x: root.visibleX(range.startUs ?? 0) - root.labelWidth
                width: root.visibleSpan(range.startUs ?? 0, range.endUs ?? 0)
                height: parent.height
                color: root.session.rangeGestureActive && !root.session.rangeGestureValid ? ExoTheme.warning : ExoTheme.accent
                opacity: 0.6
                border.color: ExoTheme.accent
                Accessible.ignored: true
            }
            MouseArea {
                id: rangeBandMouse
                objectName: "editRangeBand"
                anchors.fill: parent
                preventStealing: true
                property real pressX: 0
                property int target: EditSessionAdapter.RangeBandNone
                property bool started: false
                onPressed: mouse => {
                    if (!root.session.beginRangeBandGestureUs(root.timeAtUs(mouse.x + root.labelWidth), Math.round(6 / root.pixelsPerUs))) {
                        mouse.accepted = false;
                        return;
                    }
                    pressX = mouse.x;
                    started = false;
                    target = root.session.rangeBandTarget();
                    root.pointerScrollVertical = false;
                    root.pointerPosition = mapToItem(root, mouse.x, mouse.y);
                    root.pointerUpdater = physical => {
                        const point = rangeBandMouse.mapFromItem(root, physical.x, physical.y);
                        root.session.updateRangeBandGestureUs(root.timeAtUs(point.x + root.labelWidth));
                    };
                    root.forceActiveFocus();
                }
                onPositionChanged: mouse => {
                    if (!pressed || !root.session.rangeGestureActive)
                        return;
                    if (!started && Math.abs(mouse.x - pressX) >= Qt.styleHints.startDragDistance)
                        started = true;
                    if (started) {
                        root.pointerPosition = mapToItem(root, mouse.x, mouse.y);
                        root.pointerScrolling = true;
                        root.session.updateRangeBandGestureUs(root.timeAtUs(mouse.x + root.labelWidth));
                    }
                }
                onReleased: {
                    root.stopPointerScroll();
                    if (!root.session.rangeGestureActive)
                        return;
                    if (started)
                        root.session.commitRangeGesture();
                    else {
                        root.session.cancelRangeGesture();
                        if (target === EditSessionAdapter.RangeBandCreate)
                            root.session.clearEditRange();
                    }
                }
                onCanceled: {
                    root.stopPointerScroll();
                    root.session.cancelRangeGesture();
                }
            }
        }
        Rectangle {
            objectName: "editTrackReorderMarker"
            readonly property int from: root.trackLayout.rows.findIndex(row => row.id === root.reorderTrackId)
            readonly property var target: root.trackLayout.rows[root.reorderIndex] ?? ({top: 0, height: 0})
            visible: root.reorderTrackId !== 0 && root.reorderIndex >= 0 && root.reorderIndex !== from
            x: scroll.contentX
            y: root.rulerHeight + target.top + (root.reorderIndex > from ? target.height : 0) - 1
            width: scroll.width
            height: 2
            z: 6
            color: ExoTheme.accent
            Accessible.ignored: true
        }
        // A range marks its target tracks with a light wash and a thin bottom
        // edge, so it never competes with the clip selection's tinted headers.
        Repeater {
            model: root.displayedRange.trackIds ?? []
            delegate: Rectangle {
                required property var modelData
                readonly property var bounds: root.trackGeometry(modelData, false)
                objectName: "editRangeLane" + modelData
                x: root.visibleX(root.displayedRange.startUs ?? 0)
                y: root.rulerHeight + bounds.top
                width: root.visibleSpan(root.displayedRange.startUs ?? 0, root.displayedRange.endUs ?? 0)
                height: bounds.height
                z: 2
                color: Qt.rgba(ExoTheme.accent.r, ExoTheme.accent.g, ExoTheme.accent.b, 0.07)
                Accessible.ignored: true
                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 2
                    color: ExoTheme.accent
                }
            }
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
            id: mediaDropArea
            objectName: "editMediaDropArea"
            width: scroll.contentWidth
            height: scroll.contentHeight
            function track(drag: var): void {
                root.pointerPosition = mapToItem(root, drag.x, drag.y);
                root.pointerScrollVertical = true;
                // The drop position is read when the drag ends, so a scroll step
                // has nothing to re-project.
                root.pointerUpdater = () => {};
                root.pointerScrolling = true;
            }
            onEntered: drag => {
                root.draggingCrossfade = drag.formats.indexOf("application/x-exosnap-crossfade") >= 0;
                track(drag);
            }
            onPositionChanged: drag => track(drag)
            onExited: {
                root.draggingCrossfade = false;
                root.stopPointerScroll();
            }
            onDropped: drop => {
                root.draggingCrossfade = false;
                root.stopPointerScroll();
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
    Control {
        id: headerDivider
        objectName: "editHeaderDivider"
        x: scroll.x + root.labelWidth - width
        y: scroll.y
        width: 6
        height: scroll.height
        z: 14
        padding: 0
        activeFocusOnTab: true
        Accessible.role: Accessible.Slider
        Accessible.name: qsTr("Track header width")
        background: Rectangle { color: headerDivider.activeFocus ? ExoTheme.accent : "transparent" }
        Keys.onPressed: event => {
            if ((event.key === Qt.Key_Left || event.key === Qt.Key_Right) &&
                    (event.modifiers === Qt.NoModifier || event.modifiers === Qt.ShiftModifier)) {
                event.accepted = true;
                if (root.session.preflightPendingDraft()) {
                    const delta = event.modifiers === Qt.ShiftModifier ? 32 : 8;
                    root.userHeaderWidth = Math.max(80, Math.min(280, root.userHeaderWidth + (event.key === Qt.Key_Right ? delta : -delta)));
                    root.headerWidthEdited(root.userHeaderWidth);
                }
            }
        }
        contentItem: MouseArea {
            cursorShape: Qt.SplitHCursor
            preventStealing: true
            property real originalWidth: 168
            property real pressX: 0
            onPressed: mouse => {
                if (!root.session.preflightPendingDraft()) { mouse.accepted = false; return; }
                headerDivider.forceActiveFocus();
                originalWidth = root.userHeaderWidth;
                pressX = mapToItem(root, mouse.x, mouse.y).x;
            }
            onPositionChanged: mouse => {
                if (pressed) {
                    root.userHeaderWidth = Math.max(80, Math.min(280, originalWidth + mapToItem(root, mouse.x, mouse.y).x - pressX));
                    root.headerWidthEdited(root.userHeaderWidth);
                }
            }
            onDoubleClicked: {
                root.userHeaderWidth = 168;
                root.headerWidthEdited(168);
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
        size: Math.min(1, (viewport.endUs - viewport.startUs) / viewport.navigationEndUs)
        position: viewport.startUs / viewport.navigationEndUs
        onPositionChanged: {
            if (pressed)
                viewport.panPixels((position * viewport.navigationEndUs - viewport.startUs) * root.pixelsPerUs);
        }
    }
}
