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

    Repeater {
        model: root.settings.hotkeyRows

        ExoSettingRow {
            id: hotkeyRow

            required property var modelData

            label: hotkeyRow.modelData.label
            stacked: root.stacked
            controlWidth: 240
            Layout.fillWidth: true

            HotkeyCaptureField {
                id: shortcutField

                readonly property bool erred: root.settings.hotkeyErrorAction === hotkeyRow.modelData.action

                keys: hotkeyRow.modelData.keys
                bindingText: hotkeyRow.modelData.binding
                errorText: shortcutField.erred ? root.settings.hotkeyErrorText : ""
                errorKeys: shortcutField.erred ? root.settings.hotkeyErrorKeys : []
                modifierNames: root.settings.hotkeyModifierNames
                enabled: !root.settings.controlsLocked
                Layout.fillWidth: true
                Accessible.name: qsTr("Shortcut for %1").arg(hotkeyRow.modelData.label)
                clearAccessibleName: shortcutField.erred ? qsTr("Discard this combination") : qsTr("Clear shortcut for %1").arg(hotkeyRow.modelData.label)
                onCaptureStarted: root.settings.beginHotkeyCapture(hotkeyRow.modelData.action)
                onCaptureCancelled: root.settings.cancelHotkeyCapture()
                onCaptured: (key, modifiers) => root.settings.commitHotkeyCapture(key, modifiers)
                onCleared: root.settings.clearHotkey(hotkeyRow.modelData.action)
                onErrorDismissed: root.settings.dismissHotkeyError()
            }
        }
    }
}
