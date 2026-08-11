// Delegates below reach the card's `root` id; Bound makes that an explicit,
// compile-checked capture instead of a dynamic scope lookup.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    title: qsTr("Hotkeys")
    subtitle: qsTr("Global shortcuts stay active while ExoSnap runs in the background.")

    Repeater {
        model: root.settings.hotkeyRows

        ExoSettingRow {
            id: hotkeyRow

            required property var modelData

            readonly property bool capturing: root.settings.capturingHotkeyAction === hotkeyRow.modelData.action

            label: hotkeyRow.modelData.label
            warning: root.settings.hotkeyErrorAction === hotkeyRow.modelData.action ? root.settings.hotkeyErrorText : ""
            stacked: root.stacked
            controlWidth: 280
            Layout.fillWidth: true

            RowLayout {
                spacing: ExoTheme.spacingSm
                Layout.fillWidth: true

                HotkeyCaptureField {
                    capturing: hotkeyRow.capturing
                    binding: hotkeyRow.modelData.binding
                    enabled: !root.settings.controlsLocked
                    Layout.fillWidth: true
                    Accessible.name: qsTr("Shortcut for %1").arg(hotkeyRow.modelData.label)
                    onCaptureRequested: root.settings.beginHotkeyCapture(hotkeyRow.modelData.action)
                    onCaptureCancelled: root.settings.cancelHotkeyCapture()
                    onCaptured: (key, modifiers) => root.settings.commitHotkeyCapture(key, modifiers)
                }

                ExoButton {
                    text: qsTr("Reset")
                    quiet: true
                    enabled: !root.settings.controlsLocked && !hotkeyRow.modelData.isDefault
                    onClicked: root.settings.resetHotkey(hotkeyRow.modelData.action)
                }

                ExoButton {
                    text: qsTr("Clear")
                    quiet: true
                    enabled: !root.settings.controlsLocked && hotkeyRow.modelData.binding !== ""
                    onClicked: root.settings.clearHotkey(hotkeyRow.modelData.action)
                }
            }
        }
    }
}
