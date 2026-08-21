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
            // 300 = the capture badge's "Press a key combination..." prompt (150px at
            // 13px Hanken Grotesk, measured) plus the widest button pair sharing this
            // row -- Change (81px) + the close glyph (36px) + two 8px gaps (133px) --
            // with a small margin for font-hinting variance between platforms.
            controlWidth: 300
            Layout.fillWidth: true

            RowLayout {
                spacing: ExoTheme.spacingSm
                Layout.fillWidth: true

                HotkeyCaptureField {
                    id: captureField

                    capturing: hotkeyRow.capturing
                    binding: hotkeyRow.modelData.binding
                    enabled: !root.settings.controlsLocked
                    Layout.fillWidth: true
                    Accessible.name: qsTr("Shortcut for %1").arg(hotkeyRow.modelData.label)
                    onCaptureCancelled: root.settings.cancelHotkeyCapture()
                    onCaptured: (key, modifiers) => root.settings.commitHotkeyCapture(key, modifiers)
                }

                ExoButton {
                    text: hotkeyRow.modelData.binding === "" ? qsTr("Set") : qsTr("Change")
                    enabled: !root.settings.controlsLocked
                    onClicked: {
                        captureField.forceActiveFocus();
                        root.settings.beginHotkeyCapture(hotkeyRow.modelData.action);
                    }
                }

                ExoButton {
                    glyph: ExoGlyph.Close
                    visible: hotkeyRow.modelData.binding !== ""
                    enabled: !root.settings.controlsLocked
                    Accessible.name: qsTr("Clear shortcut for %1").arg(hotkeyRow.modelData.label)
                    onClicked: root.settings.clearHotkey(hotkeyRow.modelData.action)
                }
            }
        }
    }
}
