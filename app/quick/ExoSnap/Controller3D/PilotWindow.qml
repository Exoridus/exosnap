pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import ExoSnap.Controller3D

ApplicationWindow {
    id: window
    required property ControllerState controller
    property bool capture: false
    property bool light: false
    property bool ghost: false
    property bool pattern: false
    property bool closeup: false
    property vector3d viewRotation: Qt.vector3d(0, 0, 0)
    width: 960
    height: 640
    visible: true
    flags: window.capture ? Qt.Tool | Qt.FramelessWindowHint | Qt.WindowDoesNotAcceptFocus | Qt.WindowTransparentForInput : Qt.Window
    opacity: window.capture ? 0 : 1
    title: qsTr("DualSense native pilot")
    color: window.light ? "#edf1f5" : "#171d26"

    header: ToolBar {
        visible: !window.capture
        height: visible ? implicitHeight : 0
        RowLayout {
            anchors.fill: parent
            Label {
                text: qsTr("DualSense / procedural pilot")
                Layout.fillWidth: true
            }
            ComboBox {
                model: [qsTr("Normal 3D"), qsTr("Ghost 3D")]
                currentIndex: window.ghost ? 1 : 0
                onActivated: window.ghost = currentIndex === 1
                Accessible.name: qsTr("3D appearance")
            }
            CheckBox {
                text: qsTr("Light")
                checked: window.light
                onToggled: window.light = checked
            }
        }
    }

    Repeater {
        model: window.pattern ? 12 : 0
        Rectangle {
            required property int index
            x: index * window.width / 12
            width: window.width / 24
            height: window.height
            color: window.light ? "#b5c9d8" : "#32495e"
        }
    }

    ControllerScene {
        anchors.fill: parent
        controller: window.controller
        ghost: window.ghost
        light: window.light
        closeup: window.closeup
        viewRotation: window.viewRotation
    }

    footer: Label {
        visible: !window.capture
        height: visible ? implicitHeight + 16 : 0
        padding: 8
        color: window.light ? "#273545" : "#e5edf5"
        text: window.controller.connected ? qsTr("Live GameInput") : qsTr("No available controller input")
    }
}
