pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    required property RecordViewModelAdapter recordViewModel
    required property RecordingWorkspaceState workspaceState
    required property SettingsAdapter settings
    readonly property AudioRoutingAdapter routing: root.settings.audioRouting
    readonly property var displayedSources: root.routing.recordingActive ? root.routing.activeSources : root.routing.sources
    signal chooseSource()
    signal openProfile(int focusTarget)

    color: ExoTheme.surface
    border.color: ExoTheme.line
    radius: ExoTheme.radiusLg
    clip: true
    Accessible.name: qsTr("Recording sources")

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: ExoTheme.spacingMd
        spacing: ExoTheme.spacingMd

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: qsTr("Sources")
                font.weight: Font.DemiBold
                Layout.fillWidth: true
            }
            ExoButton {
                text: root.workspaceState.sourceState === "Pinned" ? qsTr("Unpin") : qsTr("Pin")
                enabled: root.workspaceState.sourceState === "Pinned" || root.workspaceState.canPinSources
                Accessible.name: text + qsTr(" sources panel")
                onClicked: root.workspaceState.toggleSourcePin()
            }
            ExoButton {
                text: qsTr("Close")
                Accessible.name: qsTr("Close sources panel")
                onClicked: root.workspaceState.closeSources()
            }
        }

        ExoScrollView {
            id: sourceScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            reserveScrollBarGutters: true
            contentWidth: Math.max(0, width - leftPadding - rightPadding)
            clip: true

            ColumnLayout {
                width: sourceScroll.contentWidth
                spacing: ExoTheme.spacingMd

                Label { text: qsTr("Capture source"); font.weight: Font.DemiBold }
                Label {
                    text: root.recordViewModel.sourceButtonText
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                    Layout.fillWidth: true
                    Accessible.name: qsTr("Current capture source: %1").arg(text)
                }
                ExoButton {
                    text: qsTr("Change source")
                    enabled: root.recordViewModel.canSelectSource
                    onClicked: root.chooseSource()
                }

                Label { text: qsTr("Webcam overlay"); font.weight: Font.DemiBold }
                Label {
                    text: !root.recordViewModel.webcamAvailable ? qsTr("No webcam available")
                          : root.recordViewModel.webcamError ? root.recordViewModel.webcamErrorText
                          : root.recordViewModel.webcamEnabled ? qsTr("Enabled") : qsTr("Off")
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                ExoButton {
                    text: root.recordViewModel.webcamEnabled ? qsTr("Change webcam") : qsTr("Add webcam")
                    onClicked: root.openProfile(SettingsAdapter.Webcam)
                }

                Label { text: qsTr("Audio sources"); font.weight: Font.DemiBold }
                Repeater {
                    model: root.routing.legacy ? [] : root.displayedSources
                    delegate: RowLayout {
                        id: sourceRow
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: sourceScroll.contentWidth
                        Label {
                            text: qsTr("%1: %2").arg(sourceRow.modelData.name)
                                  .arg(!sourceRow.modelData.available ? sourceRow.modelData.availabilityText
                                       : !sourceRow.modelData.participation ? qsTr("Not participating")
                                       : sourceRow.modelData.muted ? qsTr("Muted") : qsTr("Participating"))
                            textFormat: Text.PlainText
                            wrapMode: Text.WrapAnywhere
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Accessible.name: text
                        }
                        ExoButton {
                            text: sourceRow.modelData.muted ? qsTr("Unmute") : qsTr("Mute")
                            enabled: sourceRow.modelData.available && sourceRow.modelData.participation
                            Accessible.name: qsTr("%1 %2").arg(text).arg(sourceRow.modelData.name)
                            onClicked: root.routing.setSourceMute(sourceRow.modelData.id, !sourceRow.modelData.muted, root.routing.revision)
                        }
                    }
                }
                Label {
                    visible: root.routing.legacy
                    text: root.recordViewModel.systemAudioEnabled ? qsTr("System audio: participating")
                          : qsTr("System audio: muted")
                    Accessible.name: text
                }
                Label {
                    visible: root.routing.legacy && root.recordViewModel.appAudioVisible
                    text: root.recordViewModel.appAudioEnabled ? qsTr("Application audio: participating")
                          : qsTr("Application audio: muted")
                    Accessible.name: text
                }
                Label {
                    visible: root.routing.legacy
                    text: !root.recordViewModel.microphoneAvailable ? qsTr("Microphone: unavailable")
                          : root.recordViewModel.microphoneEnabled ? qsTr("Microphone: participating")
                          : qsTr("Microphone: muted")
                    Accessible.name: text
                }
                ExoButton {
                    text: qsTr("Add audio source")
                    onClicked: root.openProfile(SettingsAdapter.AudioSources)
                }
            }
        }
    }
}
