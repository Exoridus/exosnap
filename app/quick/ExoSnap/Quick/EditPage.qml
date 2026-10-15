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
    Keys.forwardTo: [clips]
    Keys.onPressed: event => {
        const focused = root.Window.window ? root.Window.window.activeFocusItem : null;
        if (focused instanceof TextInput || focused instanceof TextEdit)
            return;
        if (event.key === Qt.Key_Escape && (root.detailsOpen || root.mediaOpen)) {
            if (root.detailsOpen) root.toggleDetails();
            else root.toggleMedia();
            event.accepted = true;
        } else if ((event.modifiers & Qt.ControlModifier) !== 0 && event.key === Qt.Key_M) {
            root.toggleMedia();
            event.accepted = true;
        } else if ((event.modifiers & Qt.ControlModifier) !== 0 && event.key === Qt.Key_D) {
            root.toggleDetails();
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
    function toggleMedia(): void {
        root.mediaOpen = !root.mediaOpen;
        if (!root.mediaOpen) {
            root.mediaPinned = false;
            mediaAccess.forceActiveFocus(Qt.PopupFocusReason);
        }
        if (root.mediaOpen && root.width < 1200 && !root.detailsPinned) root.detailsOpen = false;
    }
    function toggleDetails(): void {
        root.detailsOpen = !root.detailsOpen;
        if (!root.detailsOpen) {
            root.detailsPinned = false;
            detailsAccess.forceActiveFocus(Qt.PopupFocusReason);
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
    Settings {
        id: layoutPreferences
        objectName: "editLayoutPreferences"
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
                sourceComponent: Rectangle {
                    objectName: "editMediaPanel"
                    color: ExoTheme.surface
                    radius: ExoTheme.radiusMd
                    border.color: ExoTheme.lineStrong
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: qsTr("Media"); Layout.fillWidth: true }
                            Button {
                                objectName: "editMediaPin"
                                text: root.mediaPinned ? qsTr("Unpin") : qsTr("Pin")
                                enabled: root.mediaPinned || root.canPinMedia
                                checkable: true
                                checked: root.mediaPinned
                                onClicked: root.mediaPinned = !root.mediaPinned
                            }
                            EditActionButton {
                                text: qsTr("Close Media")
                                glyph: ExoGlyph.Close
                                onClicked: root.toggleMedia()
                            }
                        }
                        EditSourceBrowser {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            session: root.session
                            recordings: root.recordings
                            timeline: root.timeline
                            currentTab: root.sourceTab
                            onCurrentTabChanged: root.sourceTab = currentTab
                        }
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
                sourceComponent: Rectangle {
                    objectName: "editDetailsPanel"
                    color: ExoTheme.surface
                    radius: ExoTheme.radiusMd
                    border.color: ExoTheme.lineStrong
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: qsTr("Details"); Layout.fillWidth: true }
                            Button {
                                objectName: "editDetailsPin"
                                text: root.detailsPinned ? qsTr("Unpin") : qsTr("Pin")
                                enabled: root.detailsPinned || root.canPinDetails
                                checkable: true
                                checked: root.detailsPinned
                                onClicked: root.detailsPinned = !root.detailsPinned
                            }
                            EditActionButton {
                                text: qsTr("Close Details")
                                glyph: ExoGlyph.Close
                                onClicked: root.toggleDetails()
                            }
                        }
                        ScrollView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            contentWidth: availableWidth
                            ColumnLayout {
                                width: parent.width
                                Label {
                                    Layout.fillWidth: true
                                    text: root.session.clipTitle
                                    wrapMode: Text.Wrap
                                }
                                Repeater {
                                    model: root.session.facts
                                    Label {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        text: qsTr("%1: %2").arg(modelData.label).arg(modelData.value)
                                        wrapMode: Text.Wrap
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: root.player.monitoringTracks.length > 0
                                    text: qsTr("Monitor tracks")
                                }
                                Repeater {
                                    model: root.player.monitoringTracks
                                    ExoCheckBox {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        text: modelData.name
                                        checked: root.monitoringSelection.indexOf(modelData.id) >= 0
                                        onToggledByUser: value => {
                                            const selected = root.monitoringSelection.slice();
                                            const index = selected.indexOf(modelData.id);
                                            if (value && index < 0) selected.push(modelData.id);
                                            else if (!value && index >= 0) selected.splice(index, 1);
                                            root.monitoringSelection = selected;
                                        }
                                    }
                                }
                                ExoCheckBox {
                                    id: overlapConfirmation
                                    Layout.fillWidth: true
                                    visible: root.player.monitoringTracks.length > 0
                                    text: qsTr("I confirm any overlapping audio in this selection")
                                }
                                Button {
                                    visible: root.player.monitoringTracks.length > 0
                                    text: qsTr("Confirm selection")
                                    onClicked: {
                                        root.player.setPlaying(false);
                                        root.player.confirmMonitoring(root.monitoringSelection, overlapConfirmation.checked);
                                    }
                                }
                                Label { text: qsTr("Export profile") }
                                ComboBox {
                                    objectName: "editExportProfiles"
                                    Layout.fillWidth: true
                                    model: root.exporter.profileOptions.filter(option => option.selectable)
                                    textRole: "label"
                                    valueRole: "value"
                                    currentIndex: model.findIndex(option => option.value === root.exporter.profileKey)
                                    Accessible.name: qsTr("Export profile")
                                    onActivated: root.exporter.profileKey = currentValue
                                }
                                Label { text: qsTr("Export range") }
                                ComboBox {
                                    objectName: "editExportSpan"
                                    Layout.fillWidth: true
                                    model: [
                                        { value: "result", label: qsTr("Result range") },
                                        { value: "entire", label: qsTr("Entire timeline") }
                                    ]
                                    textRole: "label"
                                    valueRole: "value"
                                    currentIndex: model.findIndex(option => option.value === root.exporter.spanKey)
                                    Accessible.name: qsTr("Export range")
                                    onActivated: root.exporter.spanKey = currentValue
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
                        EditActionButton { objectName: "editDelete"; text: qsTr("Delete"); shortcutText: "Delete"; glyph: ExoGlyph.Trash; enabled: root.session.selectedClip !== 0; onClicked: root.session.deleteSelected(false) }
                        EditActionButton { objectName: "editRippleDeleteAction"; text: qsTr("Ripple delete"); shortcutText: "Shift+Delete"; glyph: ExoGlyph.CloseGap; enabled: root.session.selectedClip !== 0; onClicked: root.session.deleteSelected(true) }
                    }
                    Row {
                        id: transportGroup
                        objectName: "editTransportGroup"
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 8
                        EditActionButton {
                            objectName: "editTransportPlay"
                            text: root.player.playing ? qsTr("Pause") : qsTr("Play")
                            shortcutText: "Space"
                            glyph: root.player.playing ? ExoGlyph.Pause : ExoGlyph.Run
                            enabled: root.session.durationMs > 0 && root.player.clipOpen
                            onClicked: root.player.togglePlay()
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
                                width: 160
                                padding: 12
                                focus: true
                                onOpened: volumeSlider.forceActiveFocus(Qt.PopupFocusReason)
                                onClosed: volumeButton.forceActiveFocus(Qt.PopupFocusReason)
                                Slider {
                                    id: volumeSlider
                                    objectName: "editVolumeSlider"
                                    width: parent.width
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
                    EditActionButton { id: mediaAccess; objectName: "editMediaAccess"; text: qsTr("Media"); shortcutText: "Ctrl+M"; glyph: ExoGlyph.Folder; panelAccess: true; expanded: root.mediaOpen; pinned: root.mediaPinned; onClicked: root.toggleMedia() }
                    EditActionButton { id: detailsAccess; objectName: "editDetailsAccess"; text: qsTr("Details"); shortcutText: "Ctrl+D"; glyph: ExoGlyph.Info; panelAccess: true; expanded: root.detailsOpen; pinned: root.detailsPinned; onClicked: root.toggleDetails() }
                    EditActionButton { objectName: "editSnapping"; text: qsTr("Snapping"); glyph: ExoGlyph.Magnet; checkable: true; checked: clips.snapping; onClicked: clips.snapping = checked }
                    Item { Layout.fillWidth: true }
                    EditActionButton { objectName: "editZoomOut"; text: qsTr("Zoom out"); glyph: ExoGlyph.Minus; onClicked: clips.zoomTo(clips.pixelsPerSecond / 1.25) }
                    Slider {
                        objectName: "editZoomSlider"
                        Layout.preferredWidth: root.width < 1000 ? 80 : 110
                        from: 10
                        to: 250
                        value: clips.pixelsPerSecond
                        Accessible.name: qsTr("Timeline zoom")
                        onMoved: clips.zoomTo(value)
                    }
                    EditActionButton { objectName: "editZoomIn"; text: qsTr("Zoom in"); glyph: ExoGlyph.Plus; onClicked: clips.zoomTo(clips.pixelsPerSecond * 1.25) }
                }
            }
        }
    }

    Dialog {
        id: exportReview
        objectName: "editExportReview"
        title: qsTr("Review export")
        modal: true
        width: Math.min(root.width - 32, 520)
        anchors.centerIn: parent
        closePolicy: Popup.CloseOnEscape
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
        contentItem: ColumnLayout {
            id: reviewContent
            readonly property bool planned: root.exporter.reviewOutcome.audioSummary !== undefined
            readonly property bool mixed: (root.exporter.reviewOutcome.outputAudioTracks || []).some(track => track.role === "mix")
            spacing: ExoTheme.spacingSm
            Label {
                Layout.fillWidth: true
                text: qsTr("%1 to %2 · %3 video · %4")
                    .arg(root.exporter.reviewOutcome.path === "Copy" ? qsTr("Copy") : qsTr("Render"))
                    .arg(root.exporter.reviewOutcome.container || "")
                    .arg(root.exporter.reviewOutcome.videoCodec || "")
                    .arg(root.exporter.reviewOutcome.outputFile || "")
                wrapMode: Text.Wrap
            }
            Label {
                objectName: "editExportAudioSummary"
                Layout.fillWidth: true
                visible: reviewContent.planned
                text: qsTr("Audio: %1").arg(root.exporter.reviewOutcome.audioSummary || "")
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: root.exporter.reviewOutcome.path === "Copy" && !reviewContent.planned
                text: qsTr("All %1 source audio tracks will be copied: %2")
                    .arg((root.exporter.reviewOutcome.outputAudioTracks || []).length)
                    .arg((root.exporter.reviewOutcome.outputAudioTracks || []).map(track => track.name).join(", ") || qsTr("none"))
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: reviewContent.planned ? reviewContent.mixed : root.exporter.reviewOutcome.path === "Render"
                text: root.exporter.reviewOutcome.videoOnly
                    ? qsTr("Video-only render. No audio tracks will be included.")
                    : qsTr("Selected tracks (%1) will be mixed into one 48 kHz stereo PCM24 track: %2")
                        .arg((root.exporter.reviewOutcome.selectedTracks || []).length)
                        .arg((root.exporter.reviewOutcome.selectedTracks || []).map(track => track.name).join(", "))
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: reviewContent.planned ? reviewContent.mixed : root.exporter.reviewOutcome.path === "Render"
                text: qsTr("Other tracks will not be included: %1")
                    .arg((root.exporter.reviewOutcome.excludedTracks || []).map(track => track.name).join(", ") || qsTr("none"))
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: reviewContent.planned || root.exporter.reviewOutcome.path === "Render"
                text: qsTr("Original source recordings remain unchanged.")
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: root.exporter.reviewOutcome.replaceExisting
                text: qsTr("The existing destination file will be replaced.")
                wrapMode: Text.Wrap
            }
        }
        footer: RowLayout {
            Button {
                text: qsTr("Cancel")
                onClicked: exportReview.close()
            }
            Item { Layout.fillWidth: true }
            Button {
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
