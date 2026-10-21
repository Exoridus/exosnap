import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property SettingsAdapter settings
    signal recordingProfileRequested(int focusTarget)

    readonly property real contentBox: Math.max(0, width - 2 * ExoTheme.pagePadding - ExoTheme.spacingLg)
    readonly property real contentWidth: Math.min(contentBox, ExoTheme.contentMaxWidth)
    readonly property real sideInset: Math.max(0, (contentBox - contentWidth) / 2)
    readonly property int cardColumns: ExoTheme.columnsFor(contentWidth, 2)
    readonly property bool stackedRows: ExoTheme.stackRows(contentWidth / cardColumns)
    objectName: "quickSettingsPage"

    function recordingTarget(name: string): int {
        switch (name) {
        case "preset":
        case "format": return SettingsAdapter.Format;
        case "quality": return SettingsAdapter.Quality;
        case "audio": return SettingsAdapter.AudioSources;
        case "microphone": return SettingsAdapter.Microphone;
        case "audio-encoding": return SettingsAdapter.ClockSlaving;
        case "output": return SettingsAdapter.OutputDestination;
        case "webcam": return SettingsAdapter.Webcam;
        }
        return -1;
    }

    function globalSection(name: string): var {
        switch (name) {
        case "presence": return presenceSection;
        case "overlays": return overlaysSection;
        case "hotkeys": return hotkeysSection;
        case "updates": return updatesSection;
        case "appearance": return appearanceSection;
        case "developer": return developerSection;
        }
        return null;
    }

    function revealAutomationTarget(name: string): int {
        const target = recordingTarget(name);
        if (target >= 0) {
            root.recordingProfileRequested(target);
            return 0;
        }
        const section = globalSection(name);
        return section ? (scroll.revealItem(section) ? 1 : 0) : -1;
    }

    function scrollAutomationHome(): bool { return scroll.scrollToHome(); }
    function scrollAutomationEnd(): bool { return scroll.scrollToEnd(); }
    function scrollToBottom(): void { scrollToBottomTimer.restart(); }
    function focusOutputDestination(): void {
        root.recordingProfileRequested(SettingsAdapter.OutputDestination);
    }
    function openHarnessDialog(name: string): bool {
        root.recordingProfileRequested(SettingsAdapter.Format);
        return false;
    }

    function focusGlobal(section: var, target: int): void {
        if (!section)
            return;
        scroll.revealItem(section);
        const control = typeof section.focusControl === "function" ? section.focusControl(target) : section;
        Qt.callLater(() => control.forceActiveFocus(Qt.OtherFocusReason));
    }

    Connections {
        target: root.settings
        function onSettingsFocusRequested(target: int): void {
            switch (target) {
            case SettingsAdapter.Presence: root.focusGlobal(presenceSection, target); return;
            case SettingsAdapter.Appearance: root.focusGlobal(appearanceSection, target); return;
            case SettingsAdapter.Hotkeys: root.focusGlobal(hotkeysSection, target); return;
            case SettingsAdapter.Updates: root.focusGlobal(updatesSection, target); return;
            }
        }
    }

    Timer {
        id: scrollToBottomTimer
        interval: 250
        repeat: true
        onTriggered: scroll.contentItem.contentY = Math.max(0, scroll.contentHeight - scroll.height)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: ExoTheme.pagePadding
        spacing: ExoTheme.sectionGap

        ExoScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true
            rightPadding: ExoTheme.spacingLg

            ColumnLayout {
                x: root.sideInset
                width: root.contentWidth
                spacing: ExoTheme.sectionGap

                ExoCard {
                    title: qsTr("Recording profile")
                    subtitle: qsTr("Preset, format, quality, output, webcam and audio routing")
                    Layout.fillWidth: true
                    ExoButton {
                        text: qsTr("Open recording profile")
                        Accessible.name: text
                        onClicked: root.recordingProfileRequested(SettingsAdapter.Format)
                    }
                }
                SettingsPresenceSection { id: presenceSection; settings: root.settings; stacked: root.stackedRows; Layout.fillWidth: true }
                SettingsOverlaysSection { id: overlaysSection; settings: root.settings; stacked: root.stackedRows; Layout.fillWidth: true }
                SettingsHotkeysSection { id: hotkeysSection; settings: root.settings; stacked: root.stackedRows; Layout.fillWidth: true }
                SettingsAppearanceSection { id: appearanceSection; settings: root.settings; stacked: root.stackedRows; Layout.fillWidth: true }
                SettingsStorageSection { settings: root.settings; stacked: root.stackedRows; Layout.fillWidth: true }
                SettingsUpdatesSection { id: updatesSection; settings: root.settings; stacked: root.stackedRows; Layout.fillWidth: true }
                SettingsDeveloperSection { id: developerSection; settings: root.settings; stacked: root.stackedRows; Layout.fillWidth: true }
            }
        }
    }
}
