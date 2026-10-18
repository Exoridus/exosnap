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
    property bool mediaPinned: false
    property bool detailsPinned: false
    property int sourceTab: 0
    property var monitoringSelection: []
    readonly property real panelWidth: 288
    readonly property bool canPinMedia: stage.width - root.panelWidth - 8 - (root.detailsPinned ? root.panelWidth + 8 : 0) >= 480
    readonly property bool canPinDetails: stage.width - root.panelWidth - 8 - (root.mediaPinned ? root.panelWidth + 8 : 0) >= 480
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
        } else if ((event.modifiers & Qt.ControlModifier) !== 0 && event.key === Qt.Key_M) {
            root.toggleMedia(true);
            event.accepted = true;
        } else if ((event.modifiers & Qt.ControlModifier) !== 0 && event.key === Qt.Key_D) {
            root.toggleDetails(true);
            event.accepted = true;
        } else if ((event.modifiers & Qt.ControlModifier) !== 0 && event.key === Qt.Key_E && root.session.durationMs > 0 && !root.exporter.running) {
            root.exporter.chooseDestination();
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
                root.mediaPinned = pinned;
            else if (!media && root.detailsPinned !== pinned && (!pinned || root.canPinDetails))
                root.detailsPinned = pinned;
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
    }
    // Foreign media gets new track identities on every open, so a selection
    // kept from the previous publication would no longer match any row.
    Connections {
        target: root.player
        function onMonitoringChanged(): void {
            root.monitoringSelection = root.player.monitoringTracks.filter(track => track.selected).map(track => track.id);
        }
    }
    component PanelSection: ColumnLayout {
        id: section
        property string title
        Layout.fillWidth: true
        spacing: ExoTheme.spacingSm
        Label {
            Layout.fillWidth: true
            text: section.title
            color: ExoTheme.textSecondary
            font.family: ExoTheme.sansFamily
            font.pixelSize: ExoTheme.fontCaption
            font.weight: Font.DemiBold
            Accessible.role: Accessible.Heading
        }
    }
    function toggleMedia(keyboardActivation: var): void {
        root.mediaOpen = !root.mediaOpen;
        if (!root.mediaOpen) {
            root.mediaPinned = false;
            if (keyboardActivation === true)
                mediaAccess.forceActiveFocus(Qt.TabFocusReason);
            else
                clips.forceActiveFocus(Qt.MouseFocusReason);
        }
        if (root.mediaOpen && root.width < 1200 && !root.detailsPinned) root.detailsOpen = false;
    }
    function toggleDetails(keyboardActivation: var): void {
        root.detailsOpen = !root.detailsOpen;
        if (!root.detailsOpen) {
            root.detailsPinned = false;
            if (keyboardActivation === true)
                detailsAccess.forceActiveFocus(Qt.TabFocusReason);
            else
                clips.forceActiveFocus(Qt.MouseFocusReason);
        }
        if (root.detailsOpen && root.width < 1200 && !root.mediaPinned) root.mediaOpen = false;
    }
    function restoreVertical(): void {
        if (!zones.resizing)
            lowerZone.SplitView.preferredHeight = Math.max(220, Math.min(zones.height - 170, Math.max(0, zones.height - 10) * root.fraction(layoutPreferences.timelineFraction, 0.37)));
    }
    function fraction(value: real, fallback: real): real {
        return Number.isFinite(value) && value > 0 && value < 1 ? value : fallback;
    }
    // Harness-only (--visual-test): fits the timeline to the window and opens
    // the one popup a capture asks for. The caller waits for settled geometry
    // before invoking this synchronous action. Panels arrive through the shell.
    function applyHarnessView(layout: var): void {
        if (layout.fit)
            clips.fitTimeline();
        if (layout.popup === "volume") {
            volumePopup.open();
        } else if (layout.popup === "crossfade") {
            const selected = root.session.selectedJunctionDetails;
            if (selected.outgoing !== undefined)
                clips.openTransitionPopup(selected);
        }
    }
    Settings {
        id: layoutPreferences
        objectName: "editLayoutPreferences"
        location: root.session.layoutSettingsLocation
        category: "EditLayout"
        property real sourceFraction: 0.29
        property real timelineFraction: 0.37
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
                if (!root.canPinDetails && root.detailsPinned) {
                    root.detailsPinned = false;
                    panelNotice.restart();
                }
                if (!root.canPinMedia && root.mediaPinned) {
                    root.mediaPinned = false;
                    panelNotice.restart();
                }
                if (root.width < 1200 && root.mediaOpen && root.detailsOpen && !root.mediaPinned && !root.detailsPinned)
                    root.detailsOpen = false;
            }
            EditPlayer {
                anchors.fill: parent
                anchors.leftMargin: root.mediaPinned ? root.panelWidth + 8 : 0
                anchors.rightMargin: root.detailsPinned ? root.panelWidth + 8 : 0
                session: root.session
                player: root.player
                onOpenMedia: {
                    root.sourceTab = 1;
                    if (!root.mediaOpen) root.toggleMedia();
                }
            }
            Loader {
                id: mediaPanel
                objectName: "editMediaPanelLoader"
                active: root.mediaOpen
                width: root.panelWidth
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                sourceComponent: EditSidePanel {
                    objectName: "editMediaPanel"
                    title: qsTr("Media")
                    pinned: root.mediaPinned
                    canPin: root.canPinMedia
                    pinObjectName: "editMediaPin"
                    closeText: qsTr("Close Media")
                    onPinToggled: root.mediaPinned = !root.mediaPinned
                    onCloseRequested: keyboardActivation => root.toggleMedia(keyboardActivation)
                    EditSourceBrowser {
                        anchors.fill: parent
                        session: root.session
                        recordings: root.recordings
                        timeline: root.timeline
                        currentTab: root.sourceTab
                        onCurrentTabChanged: root.sourceTab = currentTab
                    }
                }
            }
            Loader {
                objectName: "editDetailsPanelLoader"
                active: root.detailsOpen
                width: root.panelWidth
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                sourceComponent: EditSidePanel {
                    objectName: "editDetailsPanel"
                    title: qsTr("Details")
                    pinned: root.detailsPinned
                    canPin: root.canPinDetails
                    pinObjectName: "editDetailsPin"
                    closeText: qsTr("Close Details")
                    onPinToggled: root.detailsPinned = !root.detailsPinned
                    onCloseRequested: keyboardActivation => root.toggleDetails(keyboardActivation)
                    ExoScrollView {
                        id: detailsScroll
                        anchors.fill: parent
                        contentWidth: availableWidth
                        clip: true
                        // The vertical bar overlays content, so its gutter is reserved.
                        rightPadding: ExoTheme.spacingMd
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        ColumnLayout {
                            width: detailsScroll.availableWidth
                            spacing: ExoTheme.spacingLg

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: ExoTheme.spacingXs
                                Label {
                                    Layout.fillWidth: true
                                    visible: text.length > 0
                                    text: root.session.clipTitle
                                    wrapMode: Text.Wrap
                                    color: ExoTheme.text
                                    font.family: ExoTheme.sansFamily
                                    font.pixelSize: ExoTheme.fontBody
                                    font.weight: Font.DemiBold
                                }
                                Repeater {
                                    model: root.session.facts
                                    Label {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        text: qsTr("%1: %2").arg(modelData.label).arg(modelData.value)
                                        wrapMode: Text.Wrap
                                        color: ExoTheme.textSecondary
                                        font.family: ExoTheme.sansFamily
                                        font.pixelSize: ExoTheme.fontCaption
                                    }
                                }
                            }

                            PanelSection {
                                title: qsTr("Clip")
                                visible: root.session.selectedClip !== 0 || root.session.selectedTransition !== 0
                                ExoSettingRow {
                                    label: qsTr("Fade in")
                                    stacked: true
                                    visible: root.session.selectedClip !== 0
                                    Layout.fillWidth: true
                                    ExoNumberField {
                                        objectName: "editDetailsFadeIn"
                                        Layout.fillWidth: true
                                        from: 0; to: 2147483647
                                        suffix: qsTr("ms")
                                        value: root.session.selectedClipDetails.fadeInMs ?? 0
                                        Accessible.name: qsTr("Fade in duration in milliseconds")
                                        onValueCommitted: value => root.session.setClipFade(root.session.selectedClip, true, value)
                                    }
                                }
                                ExoSettingRow {
                                    label: qsTr("Fade out")
                                    stacked: true
                                    visible: root.session.selectedClip !== 0
                                    Layout.fillWidth: true
                                    ExoNumberField {
                                        objectName: "editDetailsFadeOut"
                                        Layout.fillWidth: true
                                        from: 0; to: 2147483647
                                        suffix: qsTr("ms")
                                        value: root.session.selectedClipDetails.fadeOutMs ?? 0
                                        Accessible.name: qsTr("Fade out duration in milliseconds")
                                        onValueCommitted: value => root.session.setClipFade(root.session.selectedClip, false, value)
                                    }
                                }
                                ExoSettingRow {
                                    label: qsTr("Junction start")
                                    stacked: true
                                    visible: root.session.selectedTransition !== 0
                                    Layout.fillWidth: true
                                    ExoNumberField {
                                        objectName: "editDetailsJunctionStart"
                                        Layout.fillWidth: true
                                        from: 0; to: 2147483647
                                        suffix: qsTr("ms")
                                        value: root.session.selectedJunctionDetails.startMs ?? 0
                                        Accessible.name: qsTr("Junction start in milliseconds")
                                        onValueCommitted: value => root.session.setJunctionWindow(value, root.session.selectedJunctionDetails.endMs)
                                    }
                                }
                                ExoSettingRow {
                                    label: qsTr("Junction end")
                                    stacked: true
                                    visible: root.session.selectedTransition !== 0
                                    Layout.fillWidth: true
                                    ExoNumberField {
                                        objectName: "editDetailsJunctionEnd"
                                        Layout.fillWidth: true
                                        from: 0; to: 2147483647
                                        suffix: qsTr("ms")
                                        value: root.session.selectedJunctionDetails.endMs ?? 0
                                        Accessible.name: qsTr("Junction end in milliseconds")
                                        onValueCommitted: value => root.session.setJunctionWindow(root.session.selectedJunctionDetails.startMs, value)
                                    }
                                }
                            }

                            PanelSection {
                                title: qsTr("Timeline")
                                ExoCheckBox {
                                    objectName: "editFixedTimelineEnd"
                                    Layout.fillWidth: true
                                    text: qsTr("Fixed timeline end")
                                    checked: root.session.fixedTimelineEnd
                                    onToggledByUser: value => root.session.setFixedTimelineEnd(value, root.session.durationMs)
                                }
                                ExoNumberField {
                                    objectName: "editTimelineEnd"
                                    Layout.fillWidth: true
                                    from: 0; to: 2147483647
                                    suffix: qsTr("ms")
                                    enabled: root.session.fixedTimelineEnd
                                    value: root.session.durationMs
                                    Accessible.name: qsTr("Timeline end in milliseconds")
                                    onValueCommitted: value => root.session.setFixedTimelineEnd(true, value)
                                }
                                ExoButton {
                                    objectName: "editCloseGap"
                                    Layout.fillWidth: true
                                    visible: root.session.selectedGap.endMs !== undefined
                                    text: qsTr("Close Gap (preview)")
                                    onClicked: root.session.previewCloseGap()
                                }
                                ExoButton {
                                    objectName: "editClearMediaCache"
                                    Layout.fillWidth: true
                                    text: qsTr("Clear media cache")
                                    Accessible.name: qsTr("Clear media cache")
                                    onClicked: root.timeline.clearMediaCaches()
                                    ToolTip.text: qsTr("Remove cached thumbnails and waveforms. Source recordings are kept.")
                                    ToolTip.visible: hovered
                                    ToolTip.delay: 500
                                }
                            }

                            PanelSection {
                                title: qsTr("Monitor tracks")
                                visible: root.player.monitoringTracks.length > 0
                                Repeater {
                                    model: root.player.monitoringTracks
                                    ExoCheckBox {
                                        required property var modelData
                                        required property int index
                                        Layout.fillWidth: true
                                        // Foreign media rarely names its streams.
                                        text: modelData.name.length > 0 ? modelData.name : qsTr("Track %1").arg(index + 1)
                                        checked: root.monitoringSelection.indexOf(modelData.id) >= 0
                                        onToggledByUser: value => {
                                            const selected = root.monitoringSelection.slice();
                                            const at = selected.indexOf(modelData.id);
                                            if (value && at < 0) selected.push(modelData.id);
                                            else if (!value && at >= 0) selected.splice(at, 1);
                                            root.monitoringSelection = selected;
                                        }
                                    }
                                }
                                ExoCheckBox {
                                    id: overlapConfirmation
                                    objectName: "editMonitorOverlapConfirmation"
                                    Layout.fillWidth: true
                                    visible: root.player.monitoringTracks.length > 1
                                    text: qsTr("I confirm any overlapping audio in this selection")
                                }
                                ExoButton {
                                    objectName: "editMonitorConfirm"
                                    Layout.fillWidth: true
                                    tone: root.player.monitoringConfirmationRequired ? "primary" : "neutral"
                                    text: qsTr("Confirm selection")
                                    onClicked: {
                                        root.player.setPlaying(false);
                                        // A single track cannot overlap anything; the click is the
                                        // explicit confirmation the engine requires.
                                        root.player.confirmMonitoring(root.monitoringSelection,
                                                                      overlapConfirmation.checked || root.monitoringSelection.length <= 1);
                                    }
                                }
                            }

                            PanelSection {
                                title: qsTr("Export")
                                ExoSettingRow {
                                    label: qsTr("Export profile")
                                    stacked: true
                                    Layout.fillWidth: true
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
                                    label: qsTr("Export range")
                                    stacked: true
                                    Layout.fillWidth: true
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: ExoTheme.spacingSm
                                        ExoSelect {
                                            objectName: "editExportSpan"
                                            Layout.fillWidth: true
                                            options: [
                                                { value: "entire", label: qsTr("Entire Timeline") },
                                                { value: "occupied", label: qsTr("Occupied Content") },
                                                { value: "selection", label: qsTr("Selection") }
                                            ]
                                            value: root.exporter.spanKey === "entire" || root.exporter.spanKey === "occupied" ? root.exporter.spanKey : "selection"
                                            Accessible.name: qsTr("Export range")
                                            onValueActivated: value => root.exporter.spanKey = value === "selection" ? "result" : value
                                        }
                                        ExoSelect {
                                            objectName: "editExportSelectionSpan"
                                            Layout.fillWidth: true
                                            visible: root.exporter.spanKey !== "entire" && root.exporter.spanKey !== "occupied"
                                            options: [
                                                { value: "result", label: qsTr("Result range") },
                                                { value: "time", label: qsTr("I/O time range") },
                                                { value: "selected", label: qsTr("Selected clips") }
                                            ]
                                            value: root.exporter.spanKey
                                            Accessible.name: qsTr("Selection source")
                                            onValueActivated: value => {
                                                root.exporter.setTimeSelectionUs(root.session.timeSelectionInUs, root.session.timeSelectionOutUs);
                                                root.exporter.spanKey = value;
                                            }
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
                            }
                        }
                    }
                }
                onLoaded: root.monitoringSelection = root.player.monitoringTracks.filter(track => track.selected).map(track => track.id)
            }
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 12
                text: qsTr("Panel unpinned to keep the preview usable.")
                color: ExoTheme.overlayInk
                visible: panelNotice.running
                Timer { id: panelNotice; interval: 4000 }
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
                        EditActionButton { objectName: "editUndo"; text: qsTr("Undo"); shortcutText: "Ctrl+Z"; glyph: ExoGlyph.Undo; enabled: root.session.canUndo; onClicked: root.session.undo() }
                        EditActionButton { objectName: "editRedo"; text: qsTr("Redo"); shortcutText: "Ctrl+Y"; glyph: ExoGlyph.Redo; enabled: root.session.canRedo; onClicked: root.session.redo() }
                        EditActionButton { objectName: "editSplit"; text: qsTr("Split"); shortcutText: "Ctrl+B"; glyph: ExoGlyph.Scissors; enabled: root.session.durationMs > 0; onClicked: root.session.splitSelected() }
                        EditActionButton { objectName: "editDelete"; text: qsTr("Delete"); shortcutText: "Delete"; glyph: ExoGlyph.Trash; enabled: root.session.selectedClip !== 0 || root.session.selectedSpacer !== 0; onClicked: root.session.deleteSelected(false) }
                        EditActionButton { objectName: "editRippleDeleteAction"; text: qsTr("Ripple delete"); shortcutText: "Shift+Delete"; glyph: ExoGlyph.CloseGap; enabled: root.session.selectedClip !== 0 || root.session.selectedSpacer !== 0 || root.session.selectedGap.endMs !== undefined; onClicked: root.session.deleteSelected(true) }
                    }
                    Row {
                        id: transportGroup
                        objectName: "editTransportGroup"
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 8
                        EditActionButton {
                            objectName: "editTransportPlay"
                            text: root.player.desiredPlaying ? qsTr("Pause") : qsTr("Play")
                            shortcutText: "Space"
                            glyph: root.player.desiredPlaying ? ExoGlyph.Pause : ExoGlyph.Run
                            enabled: root.session.durationMs > 0
                            onClicked: root.player.togglePlay()
                        }
                        EditActionButton {
                            objectName: "editAudioChoice"
                            text: qsTr("Choose audio tracks")
                            glyph: ExoGlyph.Speaker
                            visible: root.player.monitoringConfirmationRequired
                            onClicked: { if (!root.detailsOpen) root.toggleDetails(); }
                        }
                        Label {
                            objectName: "editTransportTimecode"
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.session.formatTimestamp(root.session.positionMs)
                            font.family: ExoTheme.monoFamily
                            font.pixelSize: ExoTheme.fontBody
                            color: ExoTheme.text
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
                            objectName: "editExportProgress"
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.exporter.state === EditExportAdapter.Preparing ? qsTr("Preparing export…") : root.exporter.state === EditExportAdapter.Cancelling ? qsTr("Cancelling…") : qsTr("%1%").arg(root.exporter.progressPercent)
                            visible: root.exporter.running
                            color: ExoTheme.textSecondary
                            Accessible.name: qsTr("Exporting %1%").arg(root.exporter.progressPercent)
                        }
                        EditActionButton { objectName: "editCancelExport"; text: qsTr("Cancel export"); glyph: ExoGlyph.Close; visible: root.exporter.running; enabled: root.exporter.state === EditExportAdapter.Running; onClicked: root.exporter.cancel() }
                        EditActionButton { objectName: "editExport"; text: qsTr("Export"); shortcutText: "Ctrl+E"; glyph: ExoGlyph.Send; tone: "primary"; enabled: root.session.durationMs > 0 && !root.exporter.running; onClicked: root.exporter.chooseDestination() }
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
                Label {
                    Layout.fillWidth: true
                    text: root.session.workspaceError || root.exporter.errorText
                    visible: text.length > 0
                    color: ExoTheme.textSecondary
                    wrapMode: Text.Wrap
                }
                EditTimeline {
                    id: clips
                    keyboardScope: root
                    session: root.session
                    player: root.player
                    thumbnails: root.timeline
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 120
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
                    EditActionButton { objectName: "editSnapping"; text: qsTr("Snapping"); glyph: ExoGlyph.Magnet; checkable: true; checked: root.session.snapping; onClicked: root.session.snapping = checked }
                    EditActionButton { objectName: "editRipple"; text: qsTr("Ripple"); glyph: ExoGlyph.CloseGap; checkable: true; checked: root.session.ripple; onClicked: root.session.ripple = checked }
                    EditActionButton { objectName: "editAutoCrossfade"; text: qsTr("Auto Crossfade"); glyph: ExoGlyph.Layers; checkable: true; checked: root.session.autoCrossfade; onClicked: root.session.autoCrossfade = checked }
                    EditActionButton { objectName: "editLinkedSelection"; text: qsTr("Linked Selection"); glyph: ExoGlyph.SplitTrack; checkable: true; checked: root.session.linkedSelection; onClicked: root.session.linkedSelection = checked }
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
                    }
                    EditActionButton { objectName: "editZoomIn"; text: qsTr("Zoom in"); glyph: ExoGlyph.Plus; onClicked: clips.zoomTo(clips.pixelsPerSecond * 1.25) }
                    EditActionButton { objectName: "editZoomFit"; text: qsTr("Fit timeline"); glyph: ExoGlyph.AppWindow; onClicked: clips.fitTimeline() }
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
