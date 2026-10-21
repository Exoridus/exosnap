pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore

FocusScope {
    id: root
    required property EditSessionAdapter session
    required property EditTimelineAdapter timeline
    required property EditPlayerAdapter player
    required property EditExportAdapter exporter
    required property RecordViewModelAdapter recordings
    required property ShellAdapter shell
    property bool mediaOpen: false
    property bool detailsOpen: false
    property bool mediaPinPreferred: layoutPreferences.mediaPinned
    property bool detailsPinPreferred: layoutPreferences.detailsPinned
    property bool mediaPinnedLast: layoutPreferences.mediaPinnedLast
    property int sourceTab: 0
    property Item mediaInvoker: null
    property Item detailsInvoker: null
    readonly property real maximumPanelWidth: Math.max(240, Math.min(560, width * 0.45))
    readonly property real mediaWidth: Math.max(240, Math.min(maximumPanelWidth, layoutPreferences.mediaWidth))
    readonly property real detailsWidth: Math.max(240, Math.min(maximumPanelWidth, layoutPreferences.detailsWidth))
    readonly property real panelWidth: mediaWidth
    readonly property bool docksAllowed: width >= 1000
    readonly property bool bothDocksFit: stage.width - mediaWidth - detailsWidth - 16 >= 480
    readonly property bool mediaPinned: mediaOpen && mediaPinPreferred && docksAllowed
        && stage.width - mediaWidth - 8 >= 480
        && (bothDocksFit || !detailsOpen || !detailsPinPreferred || !mediaPinnedLast)
    readonly property bool detailsPinned: detailsOpen && detailsPinPreferred && docksAllowed
        && stage.width - detailsWidth - 8 >= 480
        && (bothDocksFit || !mediaOpen || !mediaPinPreferred || mediaPinnedLast)
    readonly property bool canPinMedia: docksAllowed && stage.width - mediaWidth - 8 - (detailsPinned ? detailsWidth + 8 : 0) >= 480
    readonly property bool canPinDetails: docksAllowed && stage.width - detailsWidth - 8 - (mediaPinned ? mediaWidth + 8 : 0) >= 480
    onMediaPinPreferredChanged: layoutPreferences.mediaPinned = mediaPinPreferred
    onDetailsPinPreferredChanged: layoutPreferences.detailsPinned = detailsPinPreferred
    onMediaPinnedLastChanged: layoutPreferences.mediaPinnedLast = mediaPinnedLast
    objectName: "quickEditPage"
    Keys.priority: Keys.AfterItem
    Keys.forwardTo: [clips]
    Keys.onPressed: event => {
        const focused = root.Window.window ? root.Window.window.activeFocusItem : null;
        if (focused instanceof TextInput || focused instanceof TextEdit)
            return;
        if (event.key === Qt.Key_Escape && (root.detailsOpen || root.mediaOpen)) {
            if (root.detailsOpen) root.toggleDetails(true);
            else root.toggleMedia(true);
            event.accepted = true;
        } else if (!event.isAutoRepeat && event.modifiers === Qt.ControlModifier && event.key === Qt.Key_M) {
            root.toggleMedia(true);
            event.accepted = true;
        } else if (!event.isAutoRepeat && event.modifiers === Qt.ControlModifier && event.key === Qt.Key_D) {
            root.toggleDetails(true);
            event.accepted = true;
        } else if (!event.isAutoRepeat && event.modifiers === Qt.ControlModifier && event.key === Qt.Key_E && root.session.durationMs > 0 && !root.exporter.running) {
            root.openExport();
            event.accepted = true;
        }
    }
    Binding {
        target: root.timeline
        property: "trackWidth"
        value: clips.width
    }
    // The panel layout as this page decided it, for the control channel. The
    // page keeps the overlay and pin rules; requests below go through the same
    // toggles as the footer buttons and shortcuts.
    Binding {
        target: root.shell
        property: "editPanels"
        value: ({
                mediaOpen: root.mediaOpen,
                mediaPinned: root.mediaPinned,
                mediaCanPin: root.canPinMedia,
                detailsOpen: root.detailsOpen,
                detailsPinned: root.detailsPinned,
                detailsCanPin: root.canPinDetails,
                sourceTab: root.sourceTab
            })
    }
    Connections {
        target: root.shell
        function onEditPanelRequested(panel: int, open: bool, pinned: bool): void {
            const media = panel === ShellAdapter.EditMediaPanel;
            if ((media ? root.mediaOpen : root.detailsOpen) !== open) {
                if (media) root.toggleMedia();
                else root.toggleDetails();
            }
            if (!open)
                return;
            if (media && root.mediaPinned !== pinned && (!pinned || root.canPinMedia))
                root.setPanelPinned(true, pinned);
            else if (!media && root.detailsPinned !== pinned && (!pinned || root.canPinDetails))
                root.setPanelPinned(false, pinned);
        }
        function onEditSourceTabRequested(tab: int): void {
            root.sourceTab = tab;
        }
    }
    Connections {
        target: root.session
        function onTimeSelectionChanged(): void {
            root.exporter.setTimeSelectionUs(root.session.timeSelectionInUs, root.session.timeSelectionOutUs);
        }
        function onDirectoryDropRequested(requestId: var, roots: var, candidates: var, atUs: var, reason: string): void {
            if (directoryChoice.requestId !== 0)
                root.session.cancelMediaDrop(directoryChoice.requestId);
            directoryChoice.requestId = requestId;
            directoryChoice.candidateCount = candidates.length;
            directoryChoice.rootCount = roots.length;
            directoryChoice.reason = reason;
            directoryChoice.open();
        }
    }
    function toggleMedia(keyboardActivation: var): void {
        if (!root.session.preflightPendingDraft())
            return;
        if (!root.mediaOpen)
            root.mediaInvoker = root.Window.window ? root.Window.window.activeFocusItem : clips;
        root.mediaOpen = !root.mediaOpen;
        if (!root.mediaOpen) {
            if (keyboardActivation === true)
                (root.mediaInvoker || clips).forceActiveFocus(Qt.TabFocusReason);
            else
                clips.forceActiveFocus(Qt.MouseFocusReason);
        }
        if (root.mediaOpen && root.width < 1200 && !root.detailsPinned) root.detailsOpen = false;
    }
    component EditingToggle: ExoMenuItem {
        id: toggle
        checkable: true
        leftPadding: 32
        indicator: ExoGlyph {
            x: 10
            y: (toggle.height - height) / 2
            width: 14; height: 14
            kind: ExoGlyph.Check
            color: ExoTheme.accent
            visible: toggle.checked
        }
    }
    function toggleDetails(keyboardActivation: var): void {
        if (!root.session.preflightPendingDraft())
            return;
        if (!root.detailsOpen)
            root.detailsInvoker = root.Window.window ? root.Window.window.activeFocusItem : clips;
        root.detailsOpen = !root.detailsOpen;
        if (!root.detailsOpen) {
            if (keyboardActivation === true)
                (root.detailsInvoker || clips).forceActiveFocus(Qt.TabFocusReason);
            else
                clips.forceActiveFocus(Qt.MouseFocusReason);
        }
        if (root.detailsOpen && root.width < 1200 && !root.mediaPinned) root.mediaOpen = false;
    }
    function openInspector(field: string): void {
        if (!root.session.preflightPendingDraft())
            return;
        if (!root.detailsOpen)
            root.toggleDetails();
        if (root.detailsOpen && field.length > 0)
            Qt.callLater(() => root.inspectorFieldRequested(field));
    }
    signal inspectorFieldRequested(string fieldName)
    function setPanelPinned(media: bool, pinned: bool): void {
        if (!root.session.preflightPendingDraft())
            return;
        if (media) root.mediaPinPreferred = pinned;
        else root.detailsPinPreferred = pinned;
        if (pinned) root.mediaPinnedLast = media;
    }
    function resizePanel(media: bool, value: real): void {
        const bounded = Math.max(240, Math.min(root.maximumPanelWidth, value));
        if (media) layoutPreferences.mediaWidth = bounded;
        else layoutPreferences.detailsWidth = bounded;
    }
    function openExport(): void {
        if (root.session.preflightPendingDraft() && root.session.durationUs > 0 && !root.exporter.running)
            exportOptions.open();
    }
    function openSourceChoice(): void {
        if (root.session.preflightPendingDraft())
            sourceChoice.open();
    }
    component DockResize: Rectangle {
        id: divider
        required property bool media
        property real initialWidth: 300
        width: 8
        color: activeFocus ? ExoTheme.accent : pointer.containsMouse || pointer.pressed ? ExoTheme.lineStrong : "transparent"
        activeFocusOnTab: true
        Accessible.role: Accessible.Separator
        Accessible.name: media ? qsTr("Resize Media. Use Left and Right.") : qsTr("Resize Details. Use Left and Right.")
        Keys.onPressed: event => {
            if (event.key !== Qt.Key_Left && event.key !== Qt.Key_Right && event.key !== Qt.Key_Home)
                return;
            if (!root.session.preflightPendingDraft()) {
                event.accepted = true;
                return;
            }
            const step = (event.modifiers & Qt.ShiftModifier) !== 0 ? 32 : 8;
            const current = divider.media ? root.mediaWidth : root.detailsWidth;
            root.resizePanel(divider.media, event.key === Qt.Key_Home ? 300 : current + (event.key === Qt.Key_Right ? step : -step));
            event.accepted = true;
        }
        MouseArea {
            id: pointer
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.SplitHCursor
            onPressed: mouse => {
                if (!root.session.preflightPendingDraft()) {
                    mouse.accepted = false;
                    return;
                }
                divider.initialWidth = divider.media ? root.mediaWidth : root.detailsWidth;
                divider.forceActiveFocus(Qt.MouseFocusReason);
                startX = mapToItem(stage, mouse.x, mouse.y).x;
            }
            property real startX: 0
            onPositionChanged: mouse => {
                if (pressed) {
                    const delta = mapToItem(stage, mouse.x, mouse.y).x - startX;
                    root.resizePanel(divider.media, divider.initialWidth + (divider.media ? delta : -delta));
                }
            }
            onDoubleClicked: if (root.session.preflightPendingDraft()) root.resizePanel(divider.media, 300)
        }
    }
    function restoreVertical(): void {
        if (!zones.resizing)
            lowerZone.SplitView.preferredHeight = Math.max(220, Math.min(zones.height - 170, Math.max(0, zones.height - 10) * root.fraction(layoutPreferences.timelineFraction, 0.37)));
    }
    function fraction(value: real, fallback: real): real {
        return Number.isFinite(value) && value > 0 && value < 1 ? value : fallback;
    }
    // Panels arrive through the shell. A capture must reject requests that did
    // not reach their actual destination instead of saving a plausible fallback.
    function applyHarnessView(layout: var): bool {
        if (!layout || typeof layout !== "object")
            return false;
        const keys = ["fit", "media", "details", "sourceTab", "popup", "viewFromUs", "viewToUs"];
        if (Object.keys(layout).some(key => keys.indexOf(key) < 0))
            return false;
        for (const panel of ["media", "details"]) {
            if (layout[panel] === undefined)
                continue;
            if (["closed", "open", "pinned"].indexOf(layout[panel]) < 0)
                return false;
            if ((panel === "media" ? root.mediaOpen : root.detailsOpen) !== (layout[panel] !== "closed"))
                return false;
        }
        if (layout.sourceTab !== undefined && (!Number.isInteger(layout.sourceTab)
                || layout.sourceTab < 0 || layout.sourceTab > 2 || root.sourceTab !== layout.sourceTab))
            return false;
        if (layout.fit !== undefined && typeof layout.fit !== "boolean")
            return false;
        const hasInterval = layout.viewFromUs !== undefined || layout.viewToUs !== undefined;
        if (hasInterval && (!Number.isFinite(layout.viewFromUs) || !Number.isFinite(layout.viewToUs)
                || layout.viewFromUs < 0 || layout.viewToUs <= layout.viewFromUs))
            return false;
        if (layout.popup !== undefined && typeof layout.popup !== "string")
            return false;
        const popup = layout.popup || "";
        if (["", "volume", "crossfade", "source-audio"].indexOf(popup) < 0)
            return false;
        const selected = root.session.selectedJunctionDetails;
        if (popup === "crossfade" && selected.outgoing === undefined)
            return false;
        if (popup === "source-audio" && !root.session.sourceAudioChoice.active)
            return false;
        if (hasInterval)
            clips.view.setVisibleInterval(layout.viewFromUs, layout.viewToUs);
        else if (layout.fit)
            clips.fitTimeline();
        if (popup === "volume")
            volumePopup.open();
        else if (popup === "crossfade")
            clips.openTransitionPopup(selected);
        else if (popup === "source-audio")
            sourceChoice.open();
        return true;
    }
    Settings {
        id: layoutPreferences
        objectName: "editLayoutPreferences"
        location: root.session.layoutSettingsLocation
        category: "EditLayout"
        property real sourceFraction: 0.29
        property real timelineFraction: 0.37
        property real mediaWidth: 300
        property real detailsWidth: 300
        property real trackHeaderWidth: 168
        property bool mediaPinned: false
        property bool detailsPinned: false
        property bool mediaPinnedLast: true
    }
    Component.onCompleted: root.restoreVertical()
    SplitView {
        id: zones
        objectName: "editVerticalSplit"
        anchors.fill: parent
        anchors.margins: ExoTheme.spacingMd
        orientation: Qt.Vertical
        onHeightChanged: root.restoreVertical()
        onResizingChanged: if (!resizing && height > 10)
            layoutPreferences.timelineFraction = lowerZone.height / (height - 10)
        handle: Rectangle {
            objectName: "editVerticalSplitHandle"
            implicitHeight: 10
            color: SplitHandle.pressed || SplitHandle.hovered || activeFocus ? ExoTheme.line : "transparent"
            activeFocusOnTab: true
            Accessible.role: Accessible.Separator
            Accessible.name: qsTr("Resize preview and timeline. Use Up and Down.")
            HoverHandler { cursorShape: Qt.SplitVCursor }
            Keys.onUpPressed: {
                layoutPreferences.timelineFraction = Math.min(0.8, layoutPreferences.timelineFraction + 0.03);
                root.restoreVertical();
            }
            Keys.onDownPressed: {
                layoutPreferences.timelineFraction = Math.max(0.2, layoutPreferences.timelineFraction - 0.03);
                root.restoreVertical();
            }
        }
        Item {
            id: stage
            objectName: "editViewerStage"
            SplitView.fillHeight: true
            SplitView.minimumHeight: 160
            onWidthChanged: {
                if (root.width < 1200 && root.mediaOpen && root.detailsOpen && !root.mediaPinned && !root.detailsPinned)
                    root.detailsOpen = false;
            }
            EditPlayer {
                anchors.fill: parent
                anchors.leftMargin: root.mediaPinned ? root.mediaWidth + 8 : 0
                anchors.rightMargin: root.detailsPinned ? root.detailsWidth + 8 : 0
                session: root.session
                player: root.player
                onChooseAudioRequested: root.openSourceChoice()
                onOpenMedia: {
                    root.sourceTab = 1;
                    if (!root.mediaOpen) root.toggleMedia();
                }
            }
            Loader {
                id: mediaPanel
                objectName: "editMediaPanelLoader"
                active: root.mediaOpen
                width: root.mediaWidth
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                sourceComponent: EditSidePanel {
                    objectName: "editMediaPanel"
                    title: qsTr("Media")
                    pinned: root.mediaPinned
                    pinPreferred: root.mediaPinPreferred
                    canPin: root.canPinMedia
                    pinObjectName: "editMediaPin"
                    closeText: qsTr("Close Media")
                    showHeader: false
                    onPinToggled: root.setPanelPinned(true, !root.mediaPinPreferred)
                    onCloseRequested: keyboardActivation => root.toggleMedia(keyboardActivation)
                    EditSourceBrowser {
                        anchors.fill: parent
                        session: root.session
                        recordings: root.recordings
                        timeline: root.timeline
                        currentTab: root.sourceTab
                        pinned: root.mediaPinned
                        pinPreferred: root.mediaPinPreferred
                        canPin: root.canPinMedia
                        onPinToggled: root.setPanelPinned(true, !root.mediaPinPreferred)
                        onCloseRequested: keyboardActivation => root.toggleMedia(keyboardActivation)
                        onCurrentTabChanged: root.sourceTab = currentTab
                    }
                }
            }
            DockResize {
                objectName: "editMediaResize"
                media: true
                visible: root.mediaOpen
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                x: root.mediaWidth - width / 2
                z: 2
            }
            Loader {
                objectName: "editDetailsPanelLoader"
                active: root.detailsOpen
                width: root.detailsWidth
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                sourceComponent: EditSidePanel {
                    objectName: "editDetailsPanel"
                    title: qsTr("Details")
                    pinned: root.detailsPinned
                    pinPreferred: root.detailsPinPreferred
                    canPin: root.canPinDetails
                    pinObjectName: "editDetailsPin"
                    closeText: qsTr("Close Details")
                    onPinToggled: root.setPanelPinned(false, !root.detailsPinPreferred)
                    onCloseRequested: keyboardActivation => root.toggleDetails(keyboardActivation)
                    EditInspector {
                        id: inspector
                        anchors.fill: parent
                        session: root.session
                        canvasFocusItem: clips
                        Connections {
                            target: root
                            function onInspectorFieldRequested(fieldName: string): void { inspector.focusField(fieldName); }
                        }
                    }
                }
            }
            DockResize {
                objectName: "editDetailsResize"
                media: false
                visible: root.detailsOpen
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                x: parent.width - root.detailsWidth - width / 2
                z: 2
            }
        }
        Rectangle {
            id: lowerZone
            objectName: "editTimelineZone"
            SplitView.minimumHeight: 220
            SplitView.preferredHeight: 260
            color: ExoTheme.surface
            radius: ExoTheme.radiusMd
            border.color: ExoTheme.line
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                Item {
                    id: header
                    objectName: "editTimelineHeader"
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(46, actionGroup.implicitHeight + 12, transportGroup.implicitHeight + 12)
                    Row {
                        id: actionGroup
                        objectName: "editActionGroup"
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        EditActionButton { objectName: "editUndo"; text: qsTr("Undo"); shortcutText: "Ctrl+Z"; glyph: ExoGlyph.Undo; enabled: root.session.canUndo; disabledReason: qsTr("There is no edit to undo."); onClicked: root.session.undo() }
                        EditActionButton { objectName: "editRedo"; text: qsTr("Redo"); shortcutText: "Ctrl+Y"; glyph: ExoGlyph.Redo; enabled: root.session.canRedo; disabledReason: qsTr("There is no edit to redo."); onClicked: root.session.redo() }
                        EditActionButton { objectName: "editSplit"; text: qsTr("Split"); shortcutText: "Ctrl+B"; glyph: ExoGlyph.Scissors; enabled: root.session.durationUs > 0; disabledReason: qsTr("The timeline is empty."); onClicked: root.session.splitSelected() }
                        EditActionButton { objectName: "editDelete"; text: qsTr("Delete"); shortcutText: "Delete"; glyph: ExoGlyph.Trash; enabled: [EditSessionAdapter.ClipSelection, EditSessionAdapter.RangeSelection, EditSessionAdapter.JunctionSelection, EditSessionAdapter.SpacerSelection].includes(root.session.selectionMode); disabledReason: qsTr("Select clips, a range, a junction or a spacer to delete."); onClicked: root.session.deleteSelected(false) }
                        EditActionButton { objectName: "editRippleDeleteAction"; text: qsTr("Ripple delete"); shortcutText: "Shift+Delete"; glyph: ExoGlyph.CloseGap; visible: root.width >= 1000; enabled: [EditSessionAdapter.ClipSelection, EditSessionAdapter.RangeSelection, EditSessionAdapter.JunctionSelection, EditSessionAdapter.SpacerSelection, EditSessionAdapter.GapSelection].includes(root.session.selectionMode); disabledReason: qsTr("Select clips, a range, a junction, a spacer or a gap to remove."); onClicked: root.session.deleteSelected(true) }
                    }
                    Row {
                        id: transportGroup
                        objectName: "editTransportGroup"
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 8
                        EditActionButton { objectName: "editTransportStart"; text: qsTr("Timeline start"); glyph: ExoGlyph.JumpStart; enabled: root.session.durationUs > 0; disabledReason: qsTr("The timeline is empty."); onClicked: root.session.requestSeekUs(0) }
                        EditActionButton { objectName: "editTransportPreviousFrame"; text: qsTr("Previous frame"); glyph: ExoGlyph.StepBack; enabled: root.session.durationUs > 0; disabledReason: qsTr("The timeline is empty."); onClicked: root.session.requestFrameStep(-1) }
                        EditActionButton {
                            objectName: "editTransportPlay"
                            text: root.player.desiredPlaying ? qsTr("Pause") : qsTr("Play")
                            shortcutText: "Space"
                            glyph: root.player.desiredPlaying ? ExoGlyph.Pause : ExoGlyph.Run
                            enabled: root.session.durationUs > 0
                            disabledReason: qsTr("The timeline is empty.")
                            onClicked: root.player.togglePlay()
                        }
                        EditActionButton { objectName: "editTransportNextFrame"; text: qsTr("Next frame"); glyph: ExoGlyph.ArrowRight; enabled: root.session.durationUs > 0; disabledReason: qsTr("The timeline is empty."); onClicked: root.session.requestFrameStep(1) }
                        EditActionButton { objectName: "editTransportEnd"; text: qsTr("Timeline end"); glyph: ExoGlyph.Stop; enabled: root.session.durationUs > 0; disabledReason: qsTr("The timeline is empty."); onClicked: root.session.requestSeekUs(root.session.durationUs) }
                        EditActionButton {
                            objectName: "editAudioChoice"
                            text: qsTr("Choose audio tracks")
                            glyph: ExoGlyph.Speaker
                            visible: root.session.sourceAudioChoices.length > 0
                            onClicked: root.openSourceChoice()
                        }
                        EditActionButton {
                            objectName: "editLoop"
                            text: qsTr("Loop")
                            glyph: ExoGlyph.Repeat
                            shortcutText: "Ctrl+L"
                            checkable: true
                            checked: root.player.loopEnabled
                            enabled: root.session.durationUs > 0
                            disabledReason: qsTr("The timeline is empty.")
                            onClicked: root.player.loopEnabled = !root.player.loopEnabled
                        }
                        EditActionButton {
                            objectName: "editRangeOnce"
                            text: qsTr("Play range once")
                            glyph: ExoGlyph.PlayRange
                            shortcutText: "Shift+Space"
                            enabled: root.session.selectionMode === EditSessionAdapter.RangeSelection
                            disabledReason: qsTr("Select a range to play once.")
                            onClicked: root.player.playRangeOnce()
                        }
                        EditTimeField {
                            objectName: "editTransportTimecode"
                            anchors.verticalCenter: parent.verticalCenter
                            width: 132
                            session: root.session
                            canvasFocusItem: clips
                            format: 0
                            valueUs: root.session.positionUs
                            applyValue: value => root.session.applyTimeValue(0, "playhead", value)
                            Accessible.name: qsTr("Playback position")
                        }
                    }
                    Row {
                        id: outputGroup
                        objectName: "editOutputGroup"
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4
                        EditActionButton {
                            id: volumeButton
                            objectName: "editVolume"
                            text: root.player.volume === 0 ? qsTr("Preview volume (muted)") : qsTr("Preview volume")
                            glyph: ExoGlyph.Speaker
                            onClicked: volumePopup.open()
                            Popup {
                                id: volumePopup
                                objectName: "editVolumePopup"
                                y: -height - 4
                                x: volumeButton.width - width
                                width: 180
                                padding: ExoTheme.spacingMd
                                focus: true
                                background: Rectangle {
                                    color: ExoTheme.surfaceRaised
                                    border.width: 1
                                    border.color: ExoTheme.lineStrong
                                    radius: ExoTheme.radiusMd
                                }
                                onOpened: volumeSlider.forceActiveFocus(Qt.PopupFocusReason)
                                onClosed: volumeButton.forceActiveFocus(Qt.PopupFocusReason)
                                contentItem: ExoSlider {
                                    id: volumeSlider
                                    objectName: "editVolumeSlider"
                                    from: 0
                                    to: 1
                                    stepSize: 0.1
                                    value: root.player.volume
                                    Accessible.name: qsTr("Preview volume")
                                    onMoved: root.player.volume = value
                                }
                            }
                        }
                        Label {
                            id: exportProgress
                            objectName: "editExportProgress"
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.exporter.state === EditExportAdapter.Preparing ? qsTr("Preparing export…") : root.exporter.state === EditExportAdapter.Cancelling ? qsTr("Cancelling…") : qsTr("%1%").arg(root.exporter.progressPercent)
                            visible: root.exporter.running && implicitWidth + volumeButton.width + cancelExport.width + exportButton.width + 12
                                <= (header.width - transportGroup.implicitWidth) / 2 - 16
                            color: ExoTheme.textSecondary
                            Accessible.name: qsTr("Exporting %1%").arg(root.exporter.progressPercent)
                        }
                        EditActionButton { id: cancelExport; objectName: "editCancelExport"; text: qsTr("Cancel export"); glyph: ExoGlyph.Close; visible: root.exporter.running; enabled: root.exporter.state === EditExportAdapter.Running; disabledReason: root.exporter.state === EditExportAdapter.Cancelling ? qsTr("Export cancellation is already in progress.") : qsTr("Export is preparing."); onClicked: root.exporter.cancel() }
                        EditActionButton { id: exportButton; objectName: "editExport"; text: qsTr("Export"); shortcutText: "Ctrl+E"; glyph: ExoGlyph.Send; tone: "primary"; enabled: root.session.durationUs > 0 && !root.exporter.running; disabledReason: root.exporter.running ? qsTr("An export is already in progress.") : qsTr("The timeline is empty."); onClicked: root.openExport() }
                    }
                }
                Label {
                    objectName: "editDeliveryPending"
                    Layout.fillWidth: true
                    text: qsTr("MP4 delivery in progress. The finalized edit master is ready.")
                    visible: root.session.deliveryPending
                    color: ExoTheme.textSecondary
                    wrapMode: Text.Wrap
                }
                EditOverview {
                    objectName: "editOverview"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 12
                    Layout.leftMargin: clips.labelWidth
                    Layout.rightMargin: Math.max(0, clips.width - clips.labelWidth - clips.laneViewportRect.width)
                    session: root.session
                    player: root.player
                    view: clips.view
                    interaction: clips.interaction
                    canvasFocusItem: clips
                }
                EditTimeline {
                    id: clips
                    keyboardScope: root
                    session: root.session
                    player: root.player
                    thumbnails: root.timeline
                    userHeaderWidth: layoutPreferences.trackHeaderWidth
                    onHeaderWidthEdited: width => layoutPreferences.trackHeaderWidth = width
                    onExportRequested: root.openExport()
                    onMediaToggleRequested: root.toggleMedia(true)
                    onInspectorToggleRequested: root.toggleDetails(true)
                    onInspectorOpenRequested: fieldName => root.openInspector(fieldName)
                    onShortcutsRequested: shortcutHelp.open()
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 96
                }
                RowLayout {
                    objectName: "editTimelineFooter"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 42
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    spacing: 4
                    EditActionButton {
                        id: mediaAccess
                        property bool keyboardActivation: false
                        objectName: "editMediaAccess"
                        text: qsTr("Media")
                        shortcutText: "Ctrl+M"
                        glyph: ExoGlyph.Folder
                        panelAccess: true
                        expanded: root.mediaOpen
                        pinned: root.mediaPinned
                        onClicked: root.toggleMedia(mediaAccess.keyboardActivation)
                        Keys.onPressed: event => {
                            if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                mediaAccess.keyboardActivation = true;
                        }
                        PointHandler {
                            acceptedButtons: Qt.LeftButton
                            onActiveChanged: if (active) mediaAccess.keyboardActivation = false
                        }
                    }
                    EditActionButton {
                        id: detailsAccess
                        property bool keyboardActivation: false
                        objectName: "editDetailsAccess"
                        text: qsTr("Details")
                        shortcutText: "Ctrl+D"
                        glyph: ExoGlyph.Info
                        panelAccess: true
                        expanded: root.detailsOpen
                        pinned: root.detailsPinned
                        onClicked: root.toggleDetails(detailsAccess.keyboardActivation)
                        Keys.onPressed: event => {
                            if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                detailsAccess.keyboardActivation = true;
                        }
                        PointHandler {
                            acceptedButtons: Qt.LeftButton
                            onActiveChanged: if (active) detailsAccess.keyboardActivation = false
                        }
                    }
                    EditActionButton { objectName: "editSnapping"; text: qsTr("Snapping"); glyph: ExoGlyph.Magnet; visible: root.width >= 1100; checkable: true; checked: root.session.snapping; onClicked: root.session.snapping = checked }
                    EditActionButton { objectName: "editRipple"; text: qsTr("Ripple"); glyph: ExoGlyph.CloseGap; visible: root.width >= 1100; checkable: true; checked: root.session.ripple; onClicked: root.session.ripple = checked }
                    EditActionButton { objectName: "editAutoCrossfade"; text: qsTr("Auto Crossfade"); glyph: ExoGlyph.Layers; visible: root.width >= 1100; checkable: true; checked: root.session.autoCrossfade; onClicked: root.session.autoCrossfade = checked }
                    EditActionButton { objectName: "editLinkedSelection"; text: qsTr("Linked Selection"); glyph: ExoGlyph.SplitTrack; visible: root.width >= 1100; checkable: true; checked: root.session.linkedSelection; onClicked: root.session.linkedSelection = checked }
                    EditActionButton {
                        objectName: "editToolbarOverflow"
                        text: qsTr("More editing actions")
                        glyph: ExoGlyph.Overflow
                        visible: root.width < 1100
                        onClicked: editOverflow.popup()
                        ExoMenu {
                            id: editOverflow
                            EditingToggle { text: qsTr("Snapping"); checked: root.session.snapping; onTriggered: root.session.snapping = !root.session.snapping }
                            EditingToggle { text: qsTr("Ripple"); checked: root.session.ripple; onTriggered: root.session.ripple = !root.session.ripple }
                            EditingToggle { text: qsTr("Auto Crossfade"); checked: root.session.autoCrossfade; onTriggered: root.session.autoCrossfade = !root.session.autoCrossfade }
                            EditingToggle { objectName: "editOverflowLinkedSelection"; text: qsTr("Linked Selection"); checked: root.session.linkedSelection; onTriggered: root.session.linkedSelection = !root.session.linkedSelection }
                            ExoMenuItem { text: qsTr("Ripple delete"); visible: root.width < 1000; enabled: root.session.selectionMode !== EditSessionAdapter.NoSelection; onTriggered: root.session.deleteSelected(true) }
                            ExoMenuItem { text: qsTr("Fit timeline"); visible: root.width < 900; onTriggered: clips.fitTimeline() }
                            ExoMenuItem { text: qsTr("Fit selection"); onTriggered: clips.fitSelection() }
                            ExoMenuItem {
                                objectName: "editFitClips"
                                text: qsTr("Fit clips")
                                onTriggered: {
                                    const bounds = root.session.occupiedBounds();
                                    if (bounds.valid) clips.view.fitOccupied(bounds.startUs, bounds.endUs);
                                    else clips.fitTimeline();
                                }
                            }
                        }
                    }
                    Item { Layout.fillWidth: true }
                    EditActionButton { objectName: "editZoomOut"; text: qsTr("Zoom out"); glyph: ExoGlyph.Minus; onClicked: clips.zoomTo(clips.pixelsPerSecond / 1.25) }
                    ExoSlider {
                        objectName: "editZoomSlider"
                        Layout.preferredWidth: root.width < 1000 ? 80 : 110
                        from: Math.log(clips.minimumZoom)
                        to: Math.log(clips.maximumZoom)
                        value: Math.log(clips.pixelsPerSecond)
                        Accessible.name: qsTr("Timeline zoom")
                        onMoved: clips.zoomTo(Math.exp(value))
                        onPressedChanged: {
                            if (pressed) clips.view.beginViewOperation();
                            else clips.view.endViewOperation();
                        }
                    }
                    EditActionButton { objectName: "editZoomIn"; text: qsTr("Zoom in"); glyph: ExoGlyph.Plus; onClicked: clips.zoomTo(clips.pixelsPerSecond * 1.25) }
                    EditActionButton { objectName: "editZoomFit"; text: qsTr("Fit timeline"); glyph: ExoGlyph.AppWindow; visible: root.width >= 900; onClicked: clips.fitTimeline() }
                }
                Label {
                    id: selectionStatus
                    objectName: "editSelectionStatus"
                    readonly property string failureText: root.session.rangeGestureActive && !root.session.rangeGestureValid
                        ? root.session.rangeGestureReason : root.session.dragActive && !root.session.dragValid
                            ? root.session.dragReason : root.session.workspaceError || root.exporter.errorText
                    Layout.fillWidth: true
                    Layout.preferredHeight: 24
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    color: failureText.length > 0 ? ExoTheme.errorText : ExoTheme.textSecondary
                    font.family: ExoTheme.sansFamily
                    font.pixelSize: ExoTheme.fontSecondary
                    text: {
                        if (failureText.length > 0)
                            return failureText;
                        if (root.exporter.running && !exportProgress.visible)
                            return exportProgress.text;
                        if (root.session.importPending)
                            return qsTr("Importing media…");
                        if (root.session.rangeGestureActive || root.session.dragActive)
                            return qsTr("Editing selection");
                        switch (root.session.selectionMode) {
                        case EditSessionAdapter.RangeSelection:
                            return qsTr("Range %1 to %2 · %3 tracks")
                                .arg(root.session.formatTimeValue(root.session.editRange.startUs, 0))
                                .arg(root.session.formatTimeValue(root.session.editRange.endUs, 0))
                                .arg((root.session.editRange.trackIds || []).length);
                        case EditSessionAdapter.ClipSelection:
                            return qsTr("Selected clips: %1").arg(root.session.selectedClipIds.length);
                        case EditSessionAdapter.JunctionSelection: return qsTr("Junction selected");
                        case EditSessionAdapter.GapSelection: return qsTr("Gap selected");
                        case EditSessionAdapter.SpacerSelection: return qsTr("Spacer selected");
                        case EditSessionAdapter.TrackSelection:
                            return (root.session.tracks.find(track => track.id === root.session.activeTrack) || {}).name || "";
                        default: return "";
                        }
                    }
                    Accessible.name: text
                    HoverHandler { id: statusHover }
                    ToolTip.visible: statusHover.hovered && truncated
                    ToolTip.text: text
                }
            }
        }
    }

    EditSourceChoice { id: sourceChoice; session: root.session }
    Dialog {
        id: shortcutHelp
        title: qsTr("Editor shortcuts")
        modal: true
        focus: true
        anchors.centerIn: parent
        width: Math.min(root.width - 32, 520)
        standardButtons: Dialog.Close
        contentItem: ColumnLayout {
            spacing: ExoTheme.spacingSm
            Repeater {
                model: [
                    qsTr("Space: Play or pause on release"),
                    qsTr("Space + drag / wheel: Pan without changing playback"),
                    qsTr("Shift+Space: Play the active range once"),
                    qsTr("Ctrl+L: Toggle Loop"),
                    qsTr("Left / Right: Step one frame; Alt: one sample; Shift: one second"),
                    qsTr("Up / Down: Previous or next boundary"),
                    qsTr("I / O: Set range in or out; Alt+X: Clear range"),
                    qsTr("Ctrl+B: Split; Delete: Lift; Shift+Delete: Ripple delete"),
                    qsTr("Ctrl+C / X / V: Copy, cut or paste clips; Ctrl+Shift+V: Insert paste"),
                    qsTr("Ctrl+Z / Ctrl+Y: Undo or redo"),
                    qsTr("Z: Fit selection; Shift+Z: Fit timeline; Backspace: Previous view"),
                    qsTr("Ctrl+M / Ctrl+D / Ctrl+E: Media, Inspector or export")
                ]
                Label {
                    required property string modelData
                    Layout.fillWidth: true
                    text: modelData
                    color: ExoTheme.text
                    wrapMode: Text.Wrap
                }
            }
        }
    }
    Dialog {
        id: directoryChoice
        objectName: "editDirectoryDropChoice"
        property var requestId: 0
        property int candidateCount: 0
        property int rootCount: 0
        property string reason: ""
        title: qsTr("Add folders")
        modal: true
        focus: true
        anchors.centerIn: parent
        width: Math.min(root.width - 32, 500)
        function resolve(insert: bool): void {
            const id = requestId;
            requestId = 0;
            root.session.resolveMediaDrop(id, insert);
            close();
        }
        onClosed: {
            if (requestId !== 0) {
                root.session.cancelMediaDrop(requestId);
                requestId = 0;
            }
        }
        contentItem: ColumnLayout {
            spacing: ExoTheme.spacingMd
            Label {
                Layout.fillWidth: true
                text: qsTr("%1 folders contain %2 supported files directly inside them. Subfolders and linked directories are excluded.").arg(directoryChoice.rootCount).arg(directoryChoice.candidateCount)
                wrapMode: Text.Wrap
                color: ExoTheme.text
            }
            Label {
                Layout.fillWidth: true
                visible: directoryChoice.reason.length > 0
                text: directoryChoice.reason
                wrapMode: Text.Wrap
                color: ExoTheme.textSecondary
            }
            ExoButton {
                objectName: "editDirectoryBrowse"
                Layout.fillWidth: true
                text: qsTr("Add browsing roots")
                onClicked: directoryChoice.resolve(false)
            }
            ExoButton {
                objectName: "editDirectoryInsert"
                Layout.fillWidth: true
                text: qsTr("Insert %1 files").arg(directoryChoice.candidateCount)
                enabled: directoryChoice.candidateCount > 0
                onClicked: directoryChoice.resolve(true)
            }
            ExoButton {
                Layout.fillWidth: true
                text: qsTr("Cancel")
                quiet: true
                onClicked: directoryChoice.close()
            }
        }
    }
    Dialog {
        id: exportOptions
        objectName: "editExportOptions"
        title: qsTr("Review export")
        modal: true
        focus: true
        anchors.centerIn: parent
        width: Math.min(root.width - 32, 480)
        contentItem: ColumnLayout {
            spacing: ExoTheme.spacingMd
            ExoSettingRow {
                label: qsTr("Export profile"); stacked: true; Layout.fillWidth: true
                ExoSelect {
                    objectName: "editExportProfiles"
                    Layout.fillWidth: true
                    options: root.exporter.profileOptions.filter(option => option.selectable)
                    value: root.exporter.profileKey
                    Accessible.name: qsTr("Export profile")
                    onValueActivated: value => root.exporter.profileKey = value
                }
            }
            ExoSettingRow {
                label: qsTr("Export range"); stacked: true; Layout.fillWidth: true
                ExoSelect {
                    objectName: "editExportSpan"
                    Layout.fillWidth: true
                    options: [
                        {value: "entire", label: qsTr("Entire timeline"), selectable: true},
                        {value: "occupied", label: qsTr("Occupied content"), selectable: true},
                        {value: "result", label: qsTr("Result range"), selectable: true},
                        {value: "time", label: qsTr("I/O time range"), selectable: true},
                        {value: "selected", label: qsTr("Selected clips"), selectable: true}
                    ]
                    value: root.exporter.spanKey
                    Accessible.name: qsTr("Export range")
                    onValueActivated: value => {
                        root.exporter.setTimeSelectionUs(root.session.timeSelectionInUs, root.session.timeSelectionOutUs);
                        root.exporter.spanKey = value;
                    }
                }
            }
            ExoCheckBox {
                objectName: "editExportOriginalStreams"
                Layout.fillWidth: true
                text: qsTr("Keep original streams")
                checked: root.exporter.modeKey === "originalStreams"
                onToggledByUser: value => root.exporter.modeKey = value ? "originalStreams" : "finishedStereo"
            }
            ExoButton {
                Layout.fillWidth: true
                text: qsTr("Choose destination")
                tone: "primary"
                onClicked: {
                    exportOptions.close();
                    root.exporter.chooseDestination();
                }
            }
        }
    }
    component ReviewLine: Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: ExoTheme.textSecondary
        font.family: ExoTheme.sansFamily
        font.pixelSize: ExoTheme.fontSecondary
    }
    Dialog {
        id: exportReview
        objectName: "editExportReview"
        title: qsTr("Review export")
        modal: true
        focus: true
        width: Math.min(root.width - 32, 520)
        anchors.centerIn: Overlay.overlay
        padding: ExoTheme.spacingLg
        closePolicy: Popup.CloseOnEscape
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
        header: Label {
            text: exportReview.title
            textFormat: Text.PlainText
            color: ExoTheme.text
            leftPadding: ExoTheme.spacingLg
            rightPadding: ExoTheme.spacingLg
            topPadding: ExoTheme.spacingLg
            font.family: ExoTheme.sansFamily
            font.pixelSize: ExoTheme.fontSectionTitle
            font.weight: Font.DemiBold
        }
        onClosed: if (root.exporter.state === EditExportAdapter.Review)
            root.exporter.discardPreparedExport()
        Connections {
            target: root.exporter
            function onStateChanged() {
                if (root.exporter.state === EditExportAdapter.Review)
                    exportReview.open()
                else if (exportReview.opened)
                    exportReview.close()
            }
        }
        // The page is created on its first visit; an outcome already waiting
        // for review then has to be shown, not missed.
        Component.onCompleted: if (root.exporter.state === EditExportAdapter.Review)
            exportReview.open()
        contentItem: ColumnLayout {
            id: reviewContent
            readonly property bool planned: root.exporter.reviewOutcome.audioSummary !== undefined
            readonly property bool mixed: (root.exporter.reviewOutcome.outputAudioTracks || []).some(track => track.role === "mix")
            spacing: ExoTheme.spacingSm
            ReviewLine {
                color: ExoTheme.text
                text: qsTr("%1 to %2 · %3 video · %4")
                    .arg(root.exporter.reviewOutcome.path === "Copy" ? qsTr("Copy") : qsTr("Render"))
                    .arg(root.exporter.reviewOutcome.container || "")
                    .arg(root.exporter.reviewOutcome.videoCodec || "")
                    .arg(root.exporter.reviewOutcome.outputFile || "")
                wrapMode: Text.Wrap
            }
            ReviewLine {
                objectName: "editExportAudioSummary"
                visible: reviewContent.planned
                text: qsTr("Audio: %1").arg(root.exporter.reviewOutcome.audioSummary || "")
            }
            ReviewLine {
                visible: root.exporter.reviewOutcome.path === "Copy" && !reviewContent.planned
                text: qsTr("All %1 source audio tracks will be copied: %2")
                    .arg((root.exporter.reviewOutcome.outputAudioTracks || []).length)
                    .arg((root.exporter.reviewOutcome.outputAudioTracks || []).map(track => track.name).join(", ") || qsTr("none"))
            }
            ReviewLine {
                visible: reviewContent.planned ? reviewContent.mixed : root.exporter.reviewOutcome.path === "Render"
                text: root.exporter.reviewOutcome.videoOnly
                    ? qsTr("Video-only render. No audio tracks will be included.")
                    : reviewContent.planned
                        ? qsTr("Enabled audio (%1) is mixed into one 48 kHz stereo %3 track: %2")
                            .arg((root.exporter.reviewOutcome.selectedTracks || []).length)
                            .arg((root.exporter.reviewOutcome.selectedTracks || []).map(track => track.name).join(", "))
                            .arg(((root.exporter.reviewOutcome.outputAudioTracks || [])[0] || {}).codec || "")
                        : qsTr("Selected tracks (%1) will be mixed into one 48 kHz stereo PCM24 track: %2")
                            .arg((root.exporter.reviewOutcome.selectedTracks || []).length)
                            .arg((root.exporter.reviewOutcome.selectedTracks || []).map(track => track.name).join(", "))
            }
            ReviewLine {
                visible: reviewContent.planned ? reviewContent.mixed : root.exporter.reviewOutcome.path === "Render"
                text: qsTr("Other tracks will not be included: %1")
                    .arg((root.exporter.reviewOutcome.excludedTracks || []).map(track => track.name).join(", ") || qsTr("none"))
            }
            ReviewLine {
                visible: reviewContent.planned || root.exporter.reviewOutcome.path === "Render"
                text: qsTr("Original source recordings remain unchanged.")
            }
            ReviewLine {
                visible: root.exporter.reviewOutcome.replaceExisting === true
                text: qsTr("The existing destination file will be replaced.")
            }
        }
        footer: Item {
            implicitWidth: reviewButtons.implicitWidth + 2 * ExoTheme.spacingLg
            implicitHeight: reviewButtons.implicitHeight + ExoTheme.spacingLg
            RowLayout {
                id: reviewButtons
                anchors.fill: parent
                anchors.leftMargin: ExoTheme.spacingLg
                anchors.rightMargin: ExoTheme.spacingLg
                anchors.bottomMargin: ExoTheme.spacingLg
                spacing: ExoTheme.spacingSm
                Item { Layout.fillWidth: true }
                ExoButton {
                    objectName: "editExportReviewCancel"
                    text: qsTr("Cancel")
                    quiet: true
                    onClicked: exportReview.close()
                }
                ExoButton {
                    objectName: "editExportReviewConfirm"
                    tone: "primary"
                    text: reviewContent.planned ? qsTr("Export")
                        : root.exporter.reviewOutcome.path === "Render" ? qsTr("Render with selected tracks") : qsTr("Start copy")
                    onClicked: {
                        root.exporter.confirmPreparedExport()
                        exportReview.close()
                    }
                }
            }
        }
    }
}
