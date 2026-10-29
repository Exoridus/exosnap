import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property SettingsAdapter settings
    property int pendingFocusTarget: -1

    function focusTarget(target: int): void {
        const section = root.sectionForFocusTarget(target);
        const control = section && typeof section.focusControl === "function" ? section.focusControl(target) : section;
        root.focusControl(section, control);
    }

    function prepareProfileVisual(expanded: bool): bool {
        root.setAutomationGroupExpanded(root, expanded);
        return true;
    }

    function setAutomationGroupExpanded(item: Item, expanded: bool): void {
        const group = item as SettingsGroupToggle;
        if (group) {
            group.open = expanded;
            return;
        }
        const audio = item as AudioRoutingPanel;
        if (audio) {
            if (expanded) audio.harnessExpand();
            else audio.harnessCollapse();
            return;
        }
        for (const child of item.children)
            root.setAutomationGroupExpanded(child, expanded);
    }

    readonly property real contentBox: Math.max(0, root.width - 2 * ExoTheme.pagePadding - ExoTheme.spacingLg)
    readonly property real contentWidth: Math.min(root.contentBox, ExoTheme.contentMaxWidth)
    readonly property real sideInset: Math.max(0, (root.contentBox - root.contentWidth) / 2)

    readonly property int cardColumns: ExoTheme.columnsFor(root.contentWidth, 2)
    readonly property bool stackedRows: ExoTheme.stackRows(root.contentWidth / root.cardColumns)

    objectName: "quickSettingsPage"

    function scrollToBottom(): void {
        scrollToBottomTimer.restart();
    }

    readonly property var automationTargets: ({
        "preset": presetSection,
        "format": formatSection,
        "quality": qualitySection,
        "audio": audioSourcesSection,
        "microphone": root.settings.audioRouting.legacy ? microphoneSection : audioSourcesSection,
        "audio-encoding": audioEncodingSection,
        "output": outputSection,
        "webcam": webcamSection,
        "overlays": overlaysSection,
        "presence": presenceSection,
        "hotkeys": hotkeysSection,
        "updates": updatesSection,
        "appearance": appearanceSection,
        "developer": developerSection
    })

    function revealAutomationTarget(name: string): int {
        const section = root.automationTargets[name];
        if (section === undefined || section === null)
            return -1;
        return scroll.revealItem(section) ? 1 : 0;
    }

    function scrollAutomationHome(): bool {
        return scroll.scrollToHome();
    }

    function scrollAutomationEnd(): bool {
        return scroll.scrollToEnd();
    }

    function focusOutputDestination(): void {
        focusControl(outputSection, outputSection.focusControl(SettingsAdapter.OutputDestination));
    }

    function focusControl(section: var, control: var): void {
        root.landOnSection(section);
        settingsFocusTimer.section = section;
        settingsFocusTimer.control = control;
        settingsFocusTimer.remainingAttempts = 8;
        settingsFocusTimer.restart();
    }

    function sectionForFocusTarget(target: int): var {
        switch (target) {
        case SettingsAdapter.OutputDestination:
            return outputSection;
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
        case SettingsAdapter.Presence:
            return presenceSection;
        case SettingsAdapter.Appearance:
            return appearanceSection;
        case SettingsAdapter.Hotkeys:
            return hotkeysSection;
        case SettingsAdapter.Updates:
            return updatesSection;
        }
        return null;
    }

    function landOnSection(section: var): void {
        if (!section)
            return;
        scroll.revealItem(section);
        if (typeof section.land === "function")
            section.land();
    }

    Timer {
        id: settingsFocusTimer

        property int remainingAttempts: 0
        property var section: null
        property var control: null

        interval: 150
        repeat: true
        onTriggered: {
            if (control) {
                scroll.revealItem(control);
                control.forceActiveFocus(Qt.OtherFocusReason);
            } else if (section) {
                scroll.revealItem(section);
            }
            remainingAttempts -= 1;
            if (remainingAttempts <= 0)
                stop();
        }
    }

    function openHarnessDialog(name: string): bool {
        return presetSection.openHarnessDialog(name);
    }

    Timer {
        id: scrollToBottomTimer

        interval: 250
        repeat: true
        onTriggered: scroll.contentItem.contentY = Math.max(0, scroll.contentHeight - scroll.height)
    }

    Connections {
        target: root.settings

        function onSettingsFocusRequested(target: int): void {
            const section = root.sectionForFocusTarget(target);
            const control = section && typeof section.focusControl === "function" ? section.focusControl(target) : null;
            root.focusControl(section, control);
        }
    }

    Component.onCompleted: {
        if (root.pendingFocusTarget >= 0)
            root.focusTarget(root.pendingFocusTarget);
        else if (root.settings.folderValidation.length > 0)
            root.focusOutputDestination();
    }

    SettingsPresetBar {
        id: presetSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsFormatSection {
        id: formatSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsAudioSourcesSection {
        id: audioSourcesSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsMicrophoneSection {
        id: microphoneSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsOutputSection {
        id: outputSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsPresenceSection {
        id: presenceSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsHotkeysSection {
        id: hotkeysSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsAppearanceSection {
        id: appearanceSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsQualitySection {
        id: qualitySection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsAudioEncodingSection {
        id: audioEncodingSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsWebcamSection {
        id: webcamSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsOverlaysSection {
        id: overlaysSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsUpdatesSection {
        id: updatesSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsDeveloperSection {
        id: developerSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsStorageSection {
        id: storageSection
        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    ColumnLayout {
        spacing: ExoTheme.spacingMd
        anchors {
            fill: parent
            margins: ExoTheme.pagePadding
        }

        RowLayout {
            visible: root.settings.recordingActive
            Layout.fillWidth: true
            Layout.leftMargin: root.sideInset
            Layout.rightMargin: root.sideInset + ExoTheme.spacingLg
            Label {
                Layout.fillWidth: true
                text: root.settings.editingNextRecording ? qsTr("For the next recording")
                      : qsTr("Active recording (read-only): %1").arg(root.settings.activePresetName)
                color: ExoTheme.textMuted
            }
            ExoButton {
                text: root.settings.editingNextRecording ? qsTr("Show active recording") : qsTr("Edit next recording")
                onClicked: {
                    if (root.settings.editingNextRecording) root.settings.showActiveRecording();
                    else root.settings.editNextRecording();
                }
            }
        }

        ExoScrollView {
            id: scroll

            contentWidth: availableWidth
            clip: true
            rightPadding: ExoTheme.spacingLg
            Layout.fillWidth: true
            Layout.fillHeight: true

            Item {
                id: compositionHost

                x: root.sideInset
                width: root.contentWidth
                implicitHeight: root.cardColumns === 1 ? oneColumn.implicitHeight
                                                       : twoColumn.implicitHeight

                ColumnLayout {
                    id: oneColumn

                    width: compositionHost.width
                    spacing: ExoTheme.sectionGap
                    visible: root.cardColumns === 1

                    LayoutItemProxy { target: presetSection; visible: !root.settings.recordingActive || root.settings.editingNextRecording }
                    LayoutItemProxy { target: formatSection }
                    LayoutItemProxy { target: audioSourcesSection }
                    LayoutItemProxy { target: microphoneSection; visible: root.settings.audioRouting.legacy }
                    LayoutItemProxy { target: outputSection }
                    LayoutItemProxy { target: presenceSection }
                    LayoutItemProxy { target: hotkeysSection }
                    LayoutItemProxy { target: appearanceSection }
                    LayoutItemProxy { target: qualitySection }
                    LayoutItemProxy { target: audioEncodingSection }
                    LayoutItemProxy { target: webcamSection }
                    LayoutItemProxy { target: overlaysSection }
                    LayoutItemProxy { target: storageSection }
                    LayoutItemProxy { target: updatesSection }
                    LayoutItemProxy { target: developerSection }
                }

                ColumnLayout {
                    id: twoColumn

                    width: compositionHost.width
                    spacing: ExoTheme.sectionGap
                    visible: root.cardColumns > 1

                    LayoutItemProxy { target: presetSection; visible: !root.settings.recordingActive || root.settings.editingNextRecording }

                    RowLayout {
                        spacing: ExoTheme.sectionGap
                        Layout.fillWidth: true

                        ColumnLayout {
                            spacing: ExoTheme.sectionGap
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            Layout.alignment: Qt.AlignTop

                            LayoutItemProxy { target: formatSection }
                            LayoutItemProxy { target: audioSourcesSection }
                            LayoutItemProxy { target: microphoneSection; visible: root.settings.audioRouting.legacy }
                            LayoutItemProxy { target: outputSection }
                            LayoutItemProxy { target: presenceSection }
                            LayoutItemProxy { target: hotkeysSection }
                            LayoutItemProxy { target: appearanceSection }

                            Item {
                                Layout.fillHeight: true
                            }
                        }

                        ColumnLayout {
                            spacing: ExoTheme.sectionGap
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            Layout.alignment: Qt.AlignTop

                            LayoutItemProxy { target: qualitySection }
                            LayoutItemProxy { target: audioEncodingSection }
                            LayoutItemProxy { target: webcamSection }
                            LayoutItemProxy { target: overlaysSection }
                            LayoutItemProxy { target: storageSection }
                            LayoutItemProxy { target: updatesSection }
                            LayoutItemProxy { target: developerSection }

                            Item {
                                Layout.fillHeight: true
                            }
                        }
                    }
                }
            }
        }
    }
}
