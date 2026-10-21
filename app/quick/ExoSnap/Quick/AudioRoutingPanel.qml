pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ExoCard {
    id: root

    required property AudioRoutingAdapter routing
    property bool editingNext: true
    title: qsTr("Audio routing")
    subtitle: root.routing.planLabel
    property var selectedTrackBySource: ({})
    property var microphoneDeviceOptions: []

    signal harnessDetailsExpansionRequested(bool expanded)

    function harnessExpand(): void {
        root.harnessDetailsExpansionRequested(true);
    }

    function harnessCollapse(): void {
        root.harnessDetailsExpansionRequested(false);
    }

    function trackNames(ids) {
        const names = [];
        for (const track of root.routing.tracks) {
            if (ids.indexOf(track.id) >= 0)
                names.push(track.name);
        }
        return names.length ? names.join(", ") : qsTr("No assigned tracks");
    }

    Label {
        Layout.fillWidth: true
        visible: root.routing.legacy || root.routing.conversionPending
        text: root.routing.conversionText || qsTr("Legacy audio settings need conversion before they can be edited.")
        wrapMode: Text.WordWrap
        color: ExoTheme.warning
    }
    RowLayout {
        Layout.fillWidth: true
        visible: root.routing.legacy || root.routing.conversionPending
        ExoButton {
            text: qsTr("Confirm conversion")
            tone: "primary"
            enabled: !root.routing.readOnly && root.routing.conversionPending
            onClicked: root.routing.confirmLegacyConversion(root.routing.revision)
        }
        ExoButton {
            text: qsTr("Cancel")
            enabled: !root.routing.readOnly && root.routing.conversionPending
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
    Repeater {
        model: root.routing.warnings
        delegate: Label {
            required property var modelData
            Layout.fillWidth: true
            text: modelData
            wrapMode: Text.WordWrap
            color: ExoTheme.warning
            Accessible.name: text
        }
    }

    Label {
        Layout.fillWidth: true
        text: root.editingNext ? qsTr("Next recording plan") : qsTr("Active recording plan")
        color: ExoTheme.text
        font.weight: Font.DemiBold
    }
    Repeater {
        model: root.routing.tracks
        delegate: AudioTrackCard {
            required property var modelData
            Layout.fillWidth: true
            routing: root.routing
            track: modelData
        }
    }
    ExoButton {
        text: qsTr("Add empty track")
        enabled: !root.routing.readOnly
        Accessible.name: qsTr("Add an empty audio track")
        onClicked: root.routing.addEmptyTrack(root.routing.revision)
    }
    ExoButton {
        text: qsTr("Undo")
        enabled: !root.routing.readOnly && root.routing.canUndo
        onClicked: root.routing.undo(root.routing.revision)
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("Active output")
        visible: root.editingNext && root.routing.recordingActive
        color: ExoTheme.text
        font.weight: Font.DemiBold
    }
    Repeater {
        model: root.editingNext && root.routing.recordingActive ? root.routing.activeTracks : []
        delegate: Label {
            required property var modelData
            Layout.fillWidth: true
            text: qsTr("%1: %2").arg(modelData.name).arg(modelData.recording ? qsTr("Recording") : qsTr("Not recording"))
            wrapMode: Text.WordWrap
            color: ExoTheme.textMuted
        }
    }
    Label {
        Layout.fillWidth: true
        visible: root.routing.recordingActive
        text: qsTr("Live source mute")
        color: ExoTheme.text
        font.weight: Font.DemiBold
    }
    Repeater {
        model: root.routing.activeSources
        delegate: RowLayout {
            required property var modelData
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: qsTr("%1: %2").arg(modelData.name).arg(modelData.availabilityText)
                wrapMode: Text.WordWrap
                color: ExoTheme.textMuted
            }
            ExoCheckBox {
                text: qsTr("Live mute")
                checked: modelData.muted
                enabled: root.routing.recordingActive
                Accessible.name: qsTr("Mute active source %1").arg(modelData.name)
                onToggledByUser: value => root.routing.setSourceMute(modelData.id, value, root.routing.revision)
            }
        }
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("Sources")
        color: ExoTheme.text
        font.weight: Font.DemiBold
    }
    RowLayout {
        Layout.fillWidth: true
        ComboBox {
            id: newSourceKind
            Layout.fillWidth: true
            model: [qsTr("System audio"), qsTr("Microphone"), qsTr("Capture target audio"), qsTr("Independent selected application")]
            Accessible.name: qsTr("New audio source type")
            enabled: !root.routing.readOnly
        }
        ExoButton {
            text: qsTr("Add source")
            enabled: !root.routing.readOnly
            onClicked: root.routing.addSource(newSourceKind.currentIndex, newSourceKind.currentText, root.routing.revision)
        }
    }
    Label {
        Layout.fillWidth: true
        text: qsTr("New sources are unassigned. Move or add them to a track. Independent application audio binds to the currently selected capture application.")
        wrapMode: Text.WordWrap
        color: ExoTheme.textMuted
    }
    Repeater {
        model: root.routing.sources
        delegate: ColumnLayout {
            id: sourceCard
            required property var modelData
            readonly property var sourceMeter: root.routing.sourceMeters[modelData.id] || {available: false}
            property bool detailsOpen: false

            Connections {
                target: root
                function onHarnessDetailsExpansionRequested(expanded: bool): void {
                    if (sourceCard.visible)
                        sourceCard.detailsOpen = expanded;
                }
            }

            function patch(key, value) {
                const details = {};
                details[key] = value;
                root.routing.setSourceDetails(modelData.id, details, root.routing.revision);
            }
            Layout.fillWidth: true
            spacing: ExoTheme.spacingXs
            Label {
                Layout.fillWidth: true
                text: qsTr("%1 - assigned to: %2").arg(modelData.name).arg(root.trackNames(modelData.assignedTrackIds))
                wrapMode: Text.WordWrap
                color: ExoTheme.text
            }
            RowLayout {
                Layout.fillWidth: true
                ExoCheckBox {
                    text: qsTr("Participate")
                    checked: modelData.participation
                    Accessible.name: qsTr("Include source %1").arg(modelData.name)
                    enabled: !root.routing.readOnly
                    onToggledByUser: value => root.routing.setSourceParticipation(modelData.id, value, root.routing.revision)
                }
                ExoCheckBox {
                    text: qsTr("Mute")
                    checked: modelData.muted
                    visible: !root.routing.recordingActive
                    Accessible.name: qsTr("Mute source %1").arg(modelData.name)
                    enabled: !root.routing.readOnly
                    onToggledByUser: value => root.routing.setSourceMute(modelData.id, value, root.routing.revision)
                }
                Label {
                    Layout.fillWidth: true
                    text: modelData.availabilityText
                    wrapMode: Text.WordWrap
                    color: ExoTheme.textMuted
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: qsTr("Source RMS")
                    color: ExoTheme.textMuted
                }
                Label {
                    text: sourceMeter.available ? qsTr("%1 dBFS").arg(Number(sourceMeter.rmsDbfs).toFixed(1)) : qsTr("Unavailable")
                    color: ExoTheme.textMuted
                }
                Label {
                    text: qsTr("Source peak")
                    color: ExoTheme.textMuted
                }
                Label {
                    text: sourceMeter.available ? qsTr("%1 dBFS").arg(Number(sourceMeter.peakDbfs).toFixed(1)) : qsTr("Unavailable")
                    color: ExoTheme.textMuted
                }
            }
            RowLayout {
                Layout.fillWidth: true
                ComboBox {
                    id: targetTrack
                    Layout.fillWidth: true
                    Layout.minimumWidth: 100
                    Accessible.name: qsTr("Target track for %1").arg(modelData.name)
                    model: root.routing.tracks
                    textRole: "name"
                    enabled: !root.routing.readOnly && root.routing.tracks.length > 0
                    Component.onCompleted: {
                        const preferred = root.selectedTrackBySource[modelData.id];
                        const index = root.routing.tracks.findIndex(track => track.id === preferred);
                        currentIndex = index >= 0 ? index : 0;
                    }
                    onActivated: index => {
                        const selected = root.routing.tracks[index];
                        const copy = Object.assign({}, root.selectedTrackBySource);
                        copy[modelData.id] = selected.id;
                        root.selectedTrackBySource = copy;
                    }
                }
                ExoButton {
                    text: qsTr("Move here")
                    tone: "primary"
                    enabled: !root.routing.readOnly && targetTrack.currentIndex >= 0
                    Accessible.name: qsTr("Move %1 to %2").arg(modelData.name).arg(targetTrack.currentText)
                    onClicked: root.routing.assignSource(modelData.id, root.routing.tracks[targetTrack.currentIndex].id, 0, root.routing.revision)
                }
                ExoButton {
                    text: qsTr("Also add")
                    enabled: !root.routing.readOnly && targetTrack.currentIndex >= 0
                    Accessible.name: qsTr("Also add %1 to %2").arg(modelData.name).arg(targetTrack.currentText)
                    onClicked: root.routing.assignSource(modelData.id, root.routing.tracks[targetTrack.currentIndex].id, 1, root.routing.revision)
                }
            }
            ExoButton {
                text: sourceCard.detailsOpen ? qsTr("Hide source details") : qsTr("Source details")
                Accessible.name: qsTr("Details for source %1").arg(modelData.name)
                onClicked: sourceCard.detailsOpen = !sourceCard.detailsOpen
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: sourceCard.detailsOpen
                enabled: !root.routing.readOnly
                Label {
                    Layout.fillWidth: true
                    text: modelData.sharedLegacyMicrophone ? qsTr("These microphone settings are shared by the legacy sources.") : qsTr("Changes apply to this source in the next recording.")
                    wrapMode: Text.WordWrap
                    color: ExoTheme.textMuted
                }
                ExoSelect {
                    Layout.fillWidth: true
                    visible: modelData.bindingKind === 1
                    options: root.microphoneDeviceOptions
                    value: modelData.followDefault ? "" : modelData.endpointId
                    Accessible.name: qsTr("Microphone device for %1").arg(modelData.name)
                    onValueActivated: value => root.routing.setSourceDetails(modelData.id, {endpointId: value, followDefault: value === ""}, root.routing.revision)
                }
                ComboBox {
                    Layout.fillWidth: true
                    Accessible.name: qsTr("Channel mode for %1").arg(modelData.name)
                    model: [qsTr("Auto"), qsTr("Preserve stereo"), qsTr("Mono mix"), qsTr("Left channel"), qsTr("Right channel")]
                    currentIndex: modelData.channelMode
                    onActivated: index => sourceCard.patch("channelMode", index)
                }
                ExoTextField {
                    Layout.fillWidth: true
                    value: String(modelData.gainDb)
                    Accessible.name: qsTr("Source gain in dB for %1").arg(modelData.name)
                    onCommitted: value => sourceCard.patch("gainDb", Number(value))
                }
                Repeater {
                    model: [
                        {key: "hpfEnabled", label: qsTr("High-pass filter")},
                        {key: "gateEnabled", label: qsTr("Noise gate")},
                        {key: "agcEnabled", label: qsTr("Automatic gain")},
                        {key: "rnnoiseEnabled", label: qsTr("Noise suppression")}
                    ]
                    delegate: ExoCheckBox {
                        required property var modelData
                        text: modelData.label
                        checked: sourceCard.modelData[modelData.key]
                        Accessible.name: qsTr("%1 for %2").arg(modelData.label).arg(sourceCard.modelData.name)
                        onToggledByUser: value => sourceCard.patch(modelData.key, value)
                    }
                }
                Repeater {
                    model: [
                        {key: "hpfCutoffHz", label: qsTr("High-pass cutoff in Hz")},
                        {key: "gateThresholdDb", label: qsTr("Gate threshold in dB")},
                        {key: "agcTargetDb", label: qsTr("Automatic gain target in dB")}
                    ]
                    delegate: ExoTextField {
                        required property var modelData
                        Layout.fillWidth: true
                        value: String(sourceCard.modelData[modelData.key])
                        Accessible.name: qsTr("%1 for %2").arg(modelData.label).arg(sourceCard.modelData.name)
                        onCommitted: value => sourceCard.patch(modelData.key, Number(value))
                    }
                }
            }
        }
    }
}
