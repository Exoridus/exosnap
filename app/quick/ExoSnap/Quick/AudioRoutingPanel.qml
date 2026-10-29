pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ExoCard {
    id: root
    required property AudioRoutingAdapter routing
    property bool editingNext: true
    property var microphoneDeviceOptions: []
    property string selectedTrackId: ""
    property string selectedRouteId: ""
    property string actionErrorTarget: ""
    property string actionError: ""
    property var creationOptions: []
    readonly property bool showingActive: routing.recordingActive && !editingNext
    readonly property var displayedTracks: showingActive ? routing.activeTracks : routing.tracks
    readonly property var selectedTrack: displayedTracks.find(item => item.id === selectedTrackId) || ({
            routes: []
        })
    readonly property var selectedRoute: selectedTrack.routes.find(item => item.id === selectedRouteId) || ({})
    title: qsTr("Audio tracks")

    function closePopups(): void {
        sourcePopup.close();
        gainPopup.close();
        trackMenu.close();
        sourceDetails.close();
    }
    function showPopup(popup: var, anchor: Item, trackId: string): void {
        root.closePopups();
        if (root.selectedTrackId !== trackId) {
            root.actionErrorTarget = "";
            root.actionError = "";
        }
        root.selectedTrackId = trackId;
        const point = anchor.mapToItem(root, 0, anchor.height + ExoTheme.spacingXs);
        popup.x = Math.max(0, Math.min(point.x, root.width - popup.width));
        popup.y = point.y;
        popup.open();
    }
    function performOperation(target: string, operation: var): void {
        root.actionErrorTarget = "";
        root.actionError = "";
        if (!operation() && root.routing.lastError !== "") {
            root.actionErrorTarget = target;
            root.actionError = root.routing.lastError;
        }
    }
    function harnessExpand(): void {
        if (root.routing.sources.length > 0)
            root.editSource(root.routing.sources[0].id);
    }
    function harnessCollapse(): void {
        root.closePopups();
    }
    function editSource(sourceId: string): void {
        root.closePopups();
        sourceDetails.sourceId = sourceId;
        sourceDetails.x = Math.max(0, (root.width - sourceDetails.width) / 2);
        sourceDetails.y = ExoTheme.cardPadding;
        sourceDetails.open();
    }
    function createSource(kind: int, name: string): void {
        root.performOperation("create-source", () => root.routing.addSourceToTrack(kind, name, root.selectedTrackId, root.routing.revision));
    }
    onVisibleChanged: {
        if (!visible)
            root.closePopups();
    }
    Connections {
        target: root.routing
        function onChanged(): void {
            if (root.routing.readOnly || root.routing.conversionPending || (root.selectedTrackId !== "" && !root.routing.tracks.some(track => track.id === root.selectedTrackId)))
                root.closePopups();
        }
    }
    ExoButton {
        id: addTrackButton
        objectName: "audioAddTrack"
        parent: root
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: ExoTheme.cardPadding
        text: qsTr("Track")
        leadingGlyph: ExoGlyph.Plus
        compact: true
        enabled: !root.routing.readOnly
        Accessible.name: qsTr("Add recording track")
        Accessible.description: root.actionErrorTarget === "add-track" ? root.actionError : ""
        ToolTip.visible: hovered && root.actionErrorTarget === "add-track"
        ToolTip.text: root.actionError
        onClicked: root.performOperation("add-track", () => root.routing.addEmptyTrack(root.routing.revision))
        ExoFieldErrorMark {
            visible: root.actionErrorTarget === "add-track"
            anchors.right: parent.right
            anchors.bottom: parent.bottom
        }
    }
    Label {
        Layout.fillWidth: true
        visible: root.routing.recordingActive
        text: root.editingNext ? qsTr("Changes apply to the next recording") : qsTr("Active recording plan")
        color: ExoTheme.textMuted
        wrapMode: Text.WordWrap
    }
    Label {
        Layout.fillWidth: true
        visible: root.routing.conversionPending
        text: root.routing.conversionText
        wrapMode: Text.WordWrap
        color: ExoTheme.warning
    }
    RowLayout {
        visible: root.routing.conversionPending
        ExoButton {
            text: qsTr("Confirm conversion")
            tone: "primary"
            enabled: !root.routing.readOnly
            onClicked: root.performOperation("conversion", () => root.routing.confirmLegacyConversion(root.routing.revision))
        }
        ExoButton {
            text: qsTr("Cancel")
            onClicked: root.performOperation("conversion", () => root.routing.cancelLegacyConversion(root.routing.revision))
        }
    }
    Label {
        Layout.fillWidth: true
        visible: (root.actionErrorTarget === "conversion" && root.actionError !== "") || (root.routing.readOnly && root.routing.lastError !== "")
        text: root.routing.readOnly ? root.routing.lastError : root.actionError
        wrapMode: Text.WordWrap
        color: ExoTheme.error
        Accessible.role: Accessible.AlertMessage
        Accessible.name: text
    }
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 0
        Repeater {
            id: trackRepeater
            model: root.displayedTracks
            delegate: AudioTrackCard {
                required property var modelData
                Layout.fillWidth: true
                routing: root.routing
                track: modelData
                errorText: root.actionErrorTarget === modelData.id ? root.actionError : ""
                showMeter: root.showingActive
                onOperationRequested: (target, operation) => root.performOperation(target, operation)
                onSourcesRequested: (anchor, trackId) => root.showPopup(sourcePopup, anchor, trackId)
                onRouteRequested: (anchor, trackId, routeId) => {
                    root.selectedRouteId = routeId;
                    root.showPopup(gainPopup, anchor, trackId);
                }
                onMenuRequested: (anchor, trackId) => root.showPopup(trackMenu, anchor, trackId)
            }
        }
    }
    Label {
        Layout.fillWidth: true
        visible: root.displayedTracks.length === 0
        text: qsTr("Add a track to choose its audio sources.")
        color: ExoTheme.textMuted
        wrapMode: Text.WordWrap
    }
    ColumnLayout {
        Layout.fillWidth: true
        visible: root.routing.recordingActive
        Label {
            text: qsTr("Live source mute")
            color: ExoTheme.textMuted
        }
        Repeater {
            model: root.routing.activeSources
            delegate: ColumnLayout {
                id: liveSourceRow
                required property var modelData
                Layout.fillWidth: true
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: liveSourceRow.modelData.name
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: ExoTheme.textMuted
                    }
                    ExoCheckBox {
                        text: qsTr("Live mute")
                        checked: liveSourceRow.modelData.muted
                        Accessible.name: qsTr("Mute active source %1").arg(liveSourceRow.modelData.name)
                        onToggledByUser: value => root.performOperation("source:" + liveSourceRow.modelData.id, () => root.routing.setSourceMute(liveSourceRow.modelData.id, value, root.routing.revision))
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.actionErrorTarget === "source:" + liveSourceRow.modelData.id
                    text: root.actionError
                    color: ExoTheme.error
                    wrapMode: Text.WordWrap
                    Accessible.role: Accessible.AlertMessage
                }
            }
        }
    }
    Popup {
        id: sourcePopup
        parent: root
        objectName: "audioSourcePopup"
        width: Math.min(320, root.width)
        padding: ExoTheme.spacingMd
        focus: true
        onAboutToShow: root.creationOptions = root.routing.sourceCreationOptions
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: ExoTheme.surfaceRaised
            border.color: ExoTheme.lineStrong
            border.width: 1
            radius: ExoTheme.radiusMd
        }
        contentItem: ColumnLayout {
            spacing: ExoTheme.spacingXs
            Label {
                Layout.fillWidth: true
                text: qsTr("Add source to %1").arg(root.selectedTrack.name || "")
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: ExoTheme.text
                font.weight: Font.DemiBold
            }
            Repeater {
                model: root.creationOptions
                delegate: ColumnLayout {
                    id: sourceKind
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: ExoTheme.spacingXs
                    ExoButton {
                        objectName: sourceKind.modelData.kind === 0 ? "audioCreateSystemSource" : sourceKind.modelData.kind === 1 ? "audioCreateMicrophoneSource" : "audioCreateRecordedAppSource"
                        Layout.fillWidth: true
                        compact: true
                        quiet: true
                        leadingGlyph: ExoGlyph.Plus
                        text: sourceKind.modelData.name
                        enabled: !root.routing.readOnly && sourceKind.modelData.selectable
                        onClicked: root.createSource(sourceKind.modelData.kind, sourceKind.modelData.name)
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: !sourceKind.modelData.selectable
                        text: sourceKind.modelData.reason
                        color: ExoTheme.textMuted
                        wrapMode: Text.WordWrap
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                visible: root.actionErrorTarget === "create-source"
                text: root.actionError
                color: ExoTheme.error
                wrapMode: Text.WordWrap
                Accessible.role: Accessible.AlertMessage
                Accessible.name: text
            }
            Label {
                visible: root.routing.sources.length > 0
                text: qsTr("Configured sources")
                color: ExoTheme.textMuted
                font.pixelSize: ExoTheme.fontSecondary
                Layout.topMargin: ExoTheme.spacingSm
            }
            Repeater {
                model: root.routing.sources
                delegate: ColumnLayout {
                    id: sourceRow
                    required property var modelData
                    readonly property var route: root.selectedTrack.routes.find(item => item.sourceId === modelData.id)
                    Layout.fillWidth: true
                    RowLayout {
                        Layout.fillWidth: true
                        ExoGlyph {
                            kind: sourceRow.modelData.bindingKind === 1 ? ExoGlyph.Mic : ExoGlyph.Speaker
                            color: ExoTheme.textMuted
                            Layout.preferredWidth: 14
                            Layout.preferredHeight: 14
                        }
                        Label {
                            Layout.fillWidth: true
                            text: sourceRow.modelData.name
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: ExoTheme.text
                        }
                        ExoButton {
                            compact: true
                            quiet: true
                            glyph: ExoGlyph.Mixer
                            text: qsTr("Device and DSP settings for %1").arg(sourceRow.modelData.name)
                            onClicked: root.editSource(sourceRow.modelData.id)
                        }
                        ExoSwitch {
                            objectName: "audioAssign-" + sourceRow.modelData.id
                            checked: !!sourceRow.route && sourceRow.route.enabled && sourceRow.modelData.participation
                            enabled: !root.routing.readOnly
                            Accessible.name: qsTr("Assign %1 to %2").arg(sourceRow.modelData.name).arg(root.selectedTrack.name || "")
                            ToolTip.visible: hovered && !sourceRow.modelData.participation
                            ToolTip.text: qsTr("Enabling this source restores it on all assigned tracks.")
                            onToggledByUser: value => root.performOperation("source:" + sourceRow.modelData.id, () => root.routing.setTrackSourceAssigned(sourceRow.modelData.id, root.selectedTrackId, value, root.routing.revision))
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: root.actionErrorTarget === "source:" + sourceRow.modelData.id
                        text: root.actionError
                        color: ExoTheme.error
                        wrapMode: Text.WordWrap
                        Accessible.role: Accessible.AlertMessage
                        Accessible.name: text
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                visible: root.routing.sources.length === 0
                text: qsTr("No audio sources configured.")
                wrapMode: Text.WordWrap
                color: ExoTheme.textMuted
            }
        }
    }
    Popup {
        id: gainPopup
        parent: root
        objectName: "audioGainPopup"
        width: Math.min(300, root.width)
        padding: ExoTheme.spacingMd
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: ExoTheme.surfaceRaised
            border.color: ExoTheme.lineStrong
            border.width: 1
            radius: ExoTheme.radiusMd
        }
        contentItem: ColumnLayout {
            spacing: ExoTheme.spacingSm
            Label {
                Layout.fillWidth: true
                text: qsTr("%1 · %2").arg(root.selectedRoute.name || "").arg(root.selectedTrack.name || "")
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: ExoTheme.text
                font.weight: Font.DemiBold
            }
            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: qsTr("Gain")
                    color: ExoTheme.textMuted
                }
                ExoNumberField {
                    objectName: "audioRouteGain"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 140
                    from: root.routing.minimumGainDb
                    to: root.routing.maximumGainDb
                    decimals: 3
                    suffix: qsTr("dB")
                    stepSize: 0.5
                    value: root.selectedRoute.gainDb || 0
                    enabled: !root.routing.readOnly
                    resetEnabled: true
                    errorText: root.actionErrorTarget === "route:" + root.selectedRouteId ? root.actionError : ""
                    Accessible.name: qsTr("Gain for %1 on %2").arg(root.selectedRoute.name || "").arg(root.selectedTrack.name || "")
                    onValueCommitted: value => root.performOperation("route:" + root.selectedRouteId, () => root.routing.setRouteGain(root.selectedRouteId, value, root.routing.revision))
                    onResetRequested: root.performOperation("route:" + root.selectedRouteId, () => root.routing.setRouteGain(root.selectedRouteId, 0, root.routing.revision))
                }
            }
            Label {
                visible: root.selectedRoute.muted || root.selectedRoute.sourceMuted || !root.selectedRoute.participation
                Layout.fillWidth: true
                text: root.selectedRoute.muted ? qsTr("This route is muted in the saved preset. Remove and add the source to restore it.") : qsTr("This source is muted or disabled. Check its source settings.")
                wrapMode: Text.WordWrap
                color: ExoTheme.textMuted
            }
            ExoButton {
                text: qsTr("Remove from this track")
                leadingGlyph: ExoGlyph.Trash
                quiet: true
                enabled: !root.routing.readOnly
                onClicked: {
                    gainPopup.close();
                    root.performOperation(root.selectedTrackId, () => root.routing.removeRoute(root.selectedRouteId, root.routing.revision));
                }
            }
        }
    }
    ExoMenu {
        id: trackMenu
        parent: root
        objectName: "audioTrackActions"
        width: Math.min(360, root.width)
        ExoMenuItem {
            objectName: "audioRenameTrack"
            text: qsTr("Rename")
            enabled: !root.routing.readOnly
            onTriggered: {
                for (let index = 0; index < trackRepeater.count; ++index) {
                    const row = trackRepeater.itemAt(index);
                    if (row.track.id === root.selectedTrackId)
                        Qt.callLater(() => row.beginRename());
                }
            }
        }
        ExoMenuItem {
            objectName: "audioPlaybackSelection"
            text: root.routing.monitorSelectionOrigin === "default" ? qsTr("Automatic: all recorded tracks") : root.selectedTrack.defaultMonitored ? qsTr("Included in ExoSnap playback") : qsTr("Excluded from ExoSnap playback")
            enabled: false
        }
        ExoMenuItem {
            text: qsTr("Use only this track for ExoSnap playback")
            enabled: !root.routing.readOnly && !!root.selectedTrack.recordEnabled && !root.selectedTrack.empty
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Sets the recorded playback and Edit selection in ExoSnap. Other players may choose a different track.")
            onTriggered: root.performOperation(root.selectedTrackId, () => root.routing.setDefaultMonitorSelection([root.selectedTrackId], root.routing.revision))
        }
        ExoMenuItem {
            visible: root.routing.monitorConfirmationRequired
            height: visible ? implicitHeight : 0
            text: qsTr("Overlapping sources need a playback choice")
            enabled: false
        }
        ExoMenuItem {
            text: root.selectedTrack.recordEnabled ? qsTr("Disable track") : qsTr("Enable track")
            enabled: !root.routing.readOnly
            onTriggered: root.performOperation(root.selectedTrackId, () => root.routing.setTrackRecordEnabled(root.selectedTrackId, !root.selectedTrack.recordEnabled, root.routing.revision))
        }
        ExoMenuItem {
            text: qsTr("Remove track")
            objectName: "audioRequestRemoveTrack"
            enabled: !root.routing.readOnly
            onTriggered: root.performOperation(root.selectedTrackId, () => root.routing.removeTrack(root.selectedTrackId, root.routing.revision))
        }
    }

    AudioSourceDetailsPopup {
        id: sourceDetails
        parent: root
        routing: root.routing
        microphoneDeviceOptions: root.microphoneDeviceOptions
    }
}
