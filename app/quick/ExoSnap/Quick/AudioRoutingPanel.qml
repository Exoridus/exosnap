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
    property bool confirmingRemoval: false
    readonly property var selectedTrack: routing.tracks.find(item => item.id === selectedTrackId) || ({
            routes: []
        })
    readonly property var selectedRoute: selectedTrack.routes.find(item => item.id === selectedRouteId) || ({})
    title: qsTr("Audio routing")
    subtitle: root.routing.recordingActive ? (root.editingNext ? qsTr("Changes apply to the next recording") : qsTr("Active recording plan")) : qsTr("Choose audio sources for each recording track")

    function closePopups(): void {
        sourcePopup.close();
        gainPopup.close();
        trackMenu.close();
        sourceCreationMenu.close();
        sourceDetails.close();
    }
    function showPopup(popup: var, anchor: Item, trackId: string): void {
        root.closePopups();
        root.selectedTrackId = trackId;
        const point = anchor.mapToItem(root, 0, anchor.height + ExoTheme.spacingXs);
        popup.x = Math.max(0, Math.min(point.x, root.width - popup.width));
        popup.y = point.y;
        popup.open();
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
        sourceCreationMenu.close();
        if (root.routing.addSource(kind, name, root.routing.revision))
            sourcePopup.open();
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
        onClicked: root.routing.addEmptyTrack(root.routing.revision)
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
            onClicked: root.routing.confirmLegacyConversion(root.routing.revision)
        }
        ExoButton {
            text: qsTr("Cancel")
            onClicked: root.routing.cancelLegacyConversion(root.routing.revision)
        }
    }
    Label {
        Layout.fillWidth: true
        visible: root.routing.lastError !== ""
        text: root.routing.lastError
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
            model: root.routing.tracks
            delegate: AudioTrackCard {
                required property var modelData
                Layout.fillWidth: true
                routing: root.routing
                track: modelData
                onSourcesRequested: (anchor, trackId) => root.showPopup(sourcePopup, anchor, trackId)
                onRouteRequested: (anchor, trackId, routeId) => {
                    root.selectedRouteId = routeId;
                    root.showPopup(gainPopup, anchor, trackId);
                }
                onMenuRequested: (anchor, trackId) => {
                    root.confirmingRemoval = false;
                    root.showPopup(trackMenu, anchor, trackId);
                }
            }
        }
    }
    Label {
        Layout.fillWidth: true
        visible: root.routing.tracks.length === 0
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
            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: modelData.name
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: ExoTheme.textMuted
                }
                ExoCheckBox {
                    text: qsTr("Live mute")
                    checked: modelData.muted
                    Accessible.name: qsTr("Mute active source %1").arg(modelData.name)
                    onToggledByUser: value => root.routing.setSourceMute(modelData.id, value, root.routing.revision)
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
                text: qsTr("Sources for %1").arg(root.selectedTrack.name || "")
                color: ExoTheme.text
                font.weight: Font.DemiBold
            }
            Repeater {
                model: root.routing.sources
                delegate: RowLayout {
                    id: sourceRow
                    required property var modelData
                    readonly property var route: root.selectedTrack.routes.find(item => item.sourceId === modelData.id)
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
                        onToggledByUser: value => root.routing.setTrackSourceAssigned(sourceRow.modelData.id, root.selectedTrackId, value, root.routing.revision)
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
            ExoButton {
                id: createSourceButton
                objectName: "audioCreateSource"
                compact: true
                quiet: true
                leadingGlyph: ExoGlyph.Plus
                text: qsTr("Set up audio source…")
                enabled: !root.routing.readOnly
                onClicked: root.showPopup(sourceCreationMenu, createSourceButton, root.selectedTrackId)
            }
        }
    }
    ExoMenu {
        id: sourceCreationMenu
        objectName: "audioSourceCreationMenu"
        parent: root
        width: Math.min(320, root.width)
        ExoMenuItem {
            objectName: "audioCreateSystemSource"
            text: qsTr("System audio")
            onTriggered: root.createSource(0, text)
        }
        ExoMenuItem {
            text: qsTr("Microphone")
            onTriggered: root.createSource(1, text)
        }
        ExoMenuItem {
            text: qsTr("Capture target audio")
            onTriggered: root.createSource(2, text)
        }
        ExoMenuItem {
            text: qsTr("Independent selected application")
            onTriggered: root.createSource(3, text)
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
                Item {
                    Layout.fillWidth: true
                }
                Label {
                    text: qsTr("%1 dB").arg(Number(root.selectedRoute.gainDb || 0).toFixed(1))
                    color: ExoTheme.text
                }
            }
            ExoSlider {
                objectName: "audioRouteGain"
                Layout.fillWidth: true
                from: root.routing.minimumGainDb
                to: root.routing.maximumGainDb
                stepSize: 0.5
                value: root.selectedRoute.gainDb || 0
                enabled: !root.routing.readOnly
                Accessible.name: qsTr("Gain for %1 on %2").arg(root.selectedRoute.name || "").arg(root.selectedTrack.name || "")
                onMovedByUser: value => root.routing.setRouteGain(root.selectedRouteId, value, root.routing.revision)
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
                    root.routing.removeRoute(root.selectedRouteId, root.routing.revision);
                }
            }
        }
    }
    ExoMenu {
        id: trackMenu
        parent: root
        objectName: "audioTrackActions"
        width: Math.min(300, root.width)
        ExoMenuItem {
            objectName: "audioRenameTrack"
            visible: !root.confirmingRemoval
            height: root.confirmingRemoval ? 0 : implicitHeight
            text: qsTr("Rename")
            onTriggered: {
                for (let index = 0; index < trackRepeater.count; ++index) {
                    const row = trackRepeater.itemAt(index);
                    if (row.track.id === root.selectedTrackId)
                        Qt.callLater(() => row.beginRename());
                }
            }
        }
        ExoMenuItem {
            visible: !root.confirmingRemoval
            height: root.confirmingRemoval ? 0 : implicitHeight
            text: root.selectedTrack.defaultMonitored ? qsTr("Playback track") : qsTr("Set as playback track")
            enabled: !!root.selectedTrack.recordEnabled && !root.selectedTrack.empty && (!root.selectedTrack.defaultMonitored || root.routing.defaultMonitorTrackIds.length > 1)
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Default playback selection in the recorded file. This does not enable live monitoring.")
            onTriggered: {
                root.routing.setDefaultMonitorSelection([root.selectedTrackId], root.routing.revision);
            }
        }
        ExoMenuItem {
            visible: !root.confirmingRemoval
            height: root.confirmingRemoval ? 0 : implicitHeight
            text: root.selectedTrack.recordEnabled ? qsTr("Disable track") : qsTr("Enable track")
            onTriggered: root.routing.setTrackRecordEnabled(root.selectedTrackId, !root.selectedTrack.recordEnabled, root.routing.revision)
        }
        ExoMenuItem {
            visible: !root.confirmingRemoval
            height: root.confirmingRemoval ? 0 : implicitHeight
            text: qsTr("Remove track…")
            objectName: "audioRequestRemoveTrack"
            onTriggered: {
                root.confirmingRemoval = true;
                Qt.callLater(() => trackMenu.open());
            }
        }
        ExoMenuItem {
            visible: root.confirmingRemoval
            height: root.confirmingRemoval ? implicitHeight : 0
            text: qsTr("Remove track and its assignments?")
            enabled: false
        }
        ExoMenuItem {
            visible: root.confirmingRemoval
            height: root.confirmingRemoval ? implicitHeight : 0
            text: qsTr("Remove track")
            objectName: "audioConfirmRemoveTrack"
            onTriggered: root.routing.removeTrack(root.selectedTrackId, root.routing.revision)
        }
        ExoMenuItem {
            visible: root.confirmingRemoval
            height: root.confirmingRemoval ? implicitHeight : 0
            text: qsTr("Cancel")
        }
    }
    AudioSourceDetailsPopup {
        id: sourceDetails
        parent: root
        routing: root.routing
        microphoneDeviceOptions: root.microphoneDeviceOptions
    }
}
