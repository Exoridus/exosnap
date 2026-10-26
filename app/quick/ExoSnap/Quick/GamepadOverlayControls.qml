pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root
    required property GamepadOverlay overlay
    implicitWidth: openButton.implicitWidth
    implicitHeight: openButton.implicitHeight

    ExoButton {
        id: openButton
        text: qsTr("Gamepad")
        selected: root.overlay !== null && root.overlay.enabled
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
                    checked: root.overlay !== null && root.overlay.enabled
                    Accessible.name: qsTr("Gamepad overlay")
                    onToggledByUser: value => root.overlay.enabled = value
                }
            }
            ExoSelect {
                Layout.fillWidth: true
                options: root.overlay ? root.overlay.devices : []
                value: root.overlay ? root.overlay.device : ""
                placeholderText: qsTr("Automatic controller")
                Accessible.name: qsTr("Controller device")
                onValueActivated: value => root.overlay.device = value
            }
            ExoSelect {
                Layout.fillWidth: true
                options: root.overlay ? root.overlay.layouts : []
                value: root.overlay ? root.overlay.layout : -1
                Accessible.name: qsTr("Controller layout")
                onValueActivated: value => root.overlay.layout = value
            }
            ExoSelect {
                Layout.fillWidth: true
                options: [{ value: false, label: qsTr("Lines") }, { value: true, label: qsTr("Glass") }]
                value: root.overlay !== null && root.overlay.glass
                Accessible.name: qsTr("Controller style")
                onValueActivated: value => root.overlay.glass = value
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Opacity"); color: ExoTheme.text }
                ExoSlider {
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    value: root.overlay ? root.overlay.opacity : 1
                    Accessible.name: qsTr("Gamepad opacity")
                    onMovedByUser: value => root.overlay.opacity = value
                }
            }
            ExoCheckBox {
                text: qsTr("Glow")
                checked: root.overlay !== null && root.overlay.glow
                onToggledByUser: value => root.overlay.glow = value
            }
            Label {
                Layout.fillWidth: true
                text: root.overlay ? root.overlay.status : ""
                color: ExoTheme.textDim
                wrapMode: Text.WordWrap
            }
        }
    }
}
