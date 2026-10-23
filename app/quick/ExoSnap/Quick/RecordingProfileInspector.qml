import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    required property SettingsAdapter settings
    required property RecordingWorkspaceState workspaceState
    readonly property bool stackedRows: true

    color: ExoTheme.surface
    border.color: ExoTheme.line
    radius: ExoTheme.radiusLg
    clip: true
    Accessible.name: qsTr("Recording profile")

    function sectionForFocusTarget(target: int): var {
        switch (target) {
        case SettingsAdapter.OutputDestination:
        case SettingsAdapter.Resolution:
            return outputSection;
        case SettingsAdapter.FramePacing:
        case SettingsAdapter.FrameRate:
        case SettingsAdapter.Quality:
            return qualitySection;
        case SettingsAdapter.Microphone:
            return root.settings.audioRouting.legacy ? microphoneSection : audioSourcesSection;
        case SettingsAdapter.ClockSlaving:
            return audioEncodingSection;
        case SettingsAdapter.AudioSources:
            return audioSourcesSection;
        case SettingsAdapter.Format:
            return formatSection;
        case SettingsAdapter.Webcam:
            return webcamSection;
        }
        return presetSection;
    }

    function focusTarget(target: int): void {
        const section = root.sectionForFocusTarget(target);
        const control = typeof section.focusControl === "function" ? section.focusControl(target) : section;
        if (control) {
            scroll.revealItem(control);
            control.forceActiveFocus(Qt.OtherFocusReason);
        }
    }

    function revealAutomationTarget(name: string): int {
        const sections = ({ "preset": presetSection, "format": formatSection,
                            "quality": qualitySection, "audio": audioSourcesSection,
                            "microphone": microphoneSection, "audio-encoding": audioEncodingSection,
                            "output": outputSection, "webcam": webcamSection });
        const section = sections[name];
        if (!section)
            return -1;
        return section.visible && section.width > 0 && section.height > 0 && scroll.revealItem(section) ? 1 : 0;
    }

    function setAutomationDetailsExpanded(expanded: bool): void {
        root.setAutomationGroupExpanded(root, expanded);
    }

    function setAutomationGroupExpanded(item: Item, expanded: bool): void {
        if (!item.visible)
            return;
        const group = item as SettingsGroupToggle;
        if (group) {
            group.open = expanded;
            return;
        }
        const audio = item as AudioRoutingPanel;
        if (audio) {
            if (expanded)
                audio.harnessExpand();
            else
                audio.harnessCollapse();
            return;
        }
        for (const child of item.children)
            root.setAutomationGroupExpanded(child, expanded);
    }

    function openHarnessDialog(name: string): bool {
        return presetSection.openHarnessDialog(name);
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: ExoTheme.spacingMd
        spacing: ExoTheme.spacingMd

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: qsTr("Recording profile")
                font.weight: Font.DemiBold
                Layout.fillWidth: true
            }
            ExoButton {
                text: root.workspaceState.profileState === "Pinned" ? qsTr("Unpin") : qsTr("Pin")
                enabled: root.workspaceState.profileState === "Pinned" || root.workspaceState.canPinProfile
                Accessible.name: text + qsTr(" profile panel")
                onClicked: root.workspaceState.toggleProfilePin()
            }
            ExoButton {
                text: qsTr("Close")
                Accessible.name: qsTr("Close recording profile")
                onClicked: root.workspaceState.closeProfile()
            }
        }

        Label {
            Layout.fillWidth: true
            text: root.settings.recordingActive && !root.settings.editingNextRecording
                  ? qsTr("Active recording (read-only)") : qsTr("For the next recording")
            color: ExoTheme.textMuted
            Accessible.name: text
        }
        Label {
            Layout.fillWidth: true
            visible: root.settings.recordingActive && !root.settings.editingNextRecording
            text: root.settings.activePresetName
            textFormat: Text.PlainText
            elide: Text.ElideRight
            Accessible.name: qsTr("Active preset: %1").arg(text)
        }
        ExoButton {
            visible: root.settings.recordingActive
            text: root.settings.editingNextRecording ? qsTr("Show active recording")
                  : qsTr("Edit next recording")
            Accessible.name: text
            onClicked: {
                if (root.settings.editingNextRecording)
                    root.settings.showActiveRecording();
                else
                    root.settings.editNextRecording();
            }
        }

        ExoScrollView {
            id: scroll
            objectName: "recordingProfileScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            reserveScrollBarGutters: true
            contentWidth: Math.max(0, width - leftPadding - rightPadding)
            clip: true

            ColumnLayout {
                width: scroll.contentWidth
                spacing: ExoTheme.sectionGap

                SettingsPresetBar {
                    id: presetSection
                    settings: root.settings
                    stacked: true
                    visible: !root.settings.recordingActive || root.settings.editingNextRecording
                    Layout.fillWidth: true
                }
                SettingsFormatSection { id: formatSection; settings: root.settings; stacked: true; Layout.fillWidth: true }
                SettingsQualitySection { id: qualitySection; settings: root.settings; stacked: true; Layout.fillWidth: true }
                SettingsOutputSection { id: outputSection; settings: root.settings; stacked: true; Layout.fillWidth: true }
                SettingsAudioSourcesSection {
                    id: audioSourcesSection
                    settings: root.settings
                    stacked: true
                    Layout.fillWidth: true
                }
                SettingsMicrophoneSection {
                    id: microphoneSection
                    settings: root.settings
                    stacked: true
                    visible: root.settings.audioRouting.legacy
                    Layout.fillWidth: true
                }
                SettingsAudioEncodingSection {
                    id: audioEncodingSection
                    settings: root.settings
                    stacked: true
                    Layout.fillWidth: true
                }
                SettingsWebcamSection { id: webcamSection; settings: root.settings; stacked: true; Layout.fillWidth: true }
            }
        }
    }
}
