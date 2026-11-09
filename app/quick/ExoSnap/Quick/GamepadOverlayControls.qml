pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root
    required property GamepadOverlay overlay
    required property SettingsAdapter settings
    property bool showButton: true
    property bool recordScope: false
    function openOptions(): void { optionsPopup.open(); }
    implicitWidth: openButton.implicitWidth
    implicitHeight: openButton.implicitHeight

    ExoButton {
        id: openButton
        visible: root.showButton
        text: qsTr("Gamepad")
        selected: gamepadSettings.sourceState.enabled === true
        onClicked: optionsPopup.open()
    }

    Popup {
        id: optionsPopup
        objectName: "gamepadOverlayPopup"
        width: 340
        x: root.width - width
        y: -height - ExoTheme.spacingSm
        padding: ExoTheme.spacingMd
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: ExoTheme.surface
            border.color: ExoTheme.lineStrong
            radius: ExoTheme.radiusSm
        }
        contentItem: ColumnLayout {
            spacing: ExoTheme.spacingSm
            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: qsTr("Gamepad overlay")
                    color: ExoTheme.text
                    Layout.fillWidth: true
                }
                ExoSwitch {
                    checked: gamepadSettings.sourceState.enabled === true
                    Accessible.name: qsTr("Gamepad overlay")
                    enabled: gamepadSettings.sourceState.editable === true
                    onToggledByUser: value => root.settings.setVisualEnabled(
                        gamepadSettings.sourceState.sourceId, gamepadSettings.sourceState.itemId,
                        gamepadSettings.sourceState.editRevision, value, root.recordScope)
                }
            }
            VisualSettingsForm {
                id: gamepadSettings
                settings: root.settings
                sourceId: root.recordScope ? root.settings.recordGamepadSourceId : root.settings.gamepadSourceId
                recordScope: root.recordScope
                stacked: true
                Layout.fillWidth: true
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Opacity"); color: ExoTheme.text }
                ExoSlider {
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    value: gamepadSettings.sourceState.opacity ?? 1
                    Accessible.name: qsTr("Gamepad opacity")
                    enabled: gamepadSettings.sourceState.editable === true
                    onMovedByUser: value => root.settings.setVisualOpacity(
                        gamepadSettings.sourceState.sourceId, gamepadSettings.sourceState.itemId,
                        gamepadSettings.sourceState.editRevision, value, root.recordScope)
                }
            }
            Label {
                Layout.fillWidth: true
                text: root.overlay ? root.overlay.status : ""
                color: ExoTheme.textMuted
                wrapMode: Text.WordWrap
            }
        }
    }
}
