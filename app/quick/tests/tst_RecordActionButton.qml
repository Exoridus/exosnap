import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

Item {
    id: root

    width: 240
    height: 120

    Component {
        id: actionButtonComponent

        RecordActionButton {
            required property bool activated

            width: 140
            height: 44
            accessibleLabel: qsTr("Start recording")
            text: qsTr("Record")
            onClicked: activated = true
        }
    }

    TestCase {
        name: "RecordActionButtonTests"
        when: windowShown

        function test_presentationProperties() {
            let button = createTemporaryObject(actionButtonComponent, root, { activated: false })
            verify(!!button, "Component exists")
            compare(button.text, qsTr("Record"))
            compare(button.Accessible.name, qsTr("Start recording"))
            compare(button.round, true)
        }

        function test_mouseActivation() {
            let button = createTemporaryObject(actionButtonComponent, root, { activated: false })
            verify(!!button, "Component exists")
            mouseClick(button)
            tryCompare(button, "activated", true)
        }

        function test_keyboardActivation() {
            let button = createTemporaryObject(actionButtonComponent, root, { activated: false })
            verify(!!button, "Component exists")
            button.forceActiveFocus()
            keyClick(Qt.Key_Space)
            tryCompare(button, "activated", true)
        }

        function test_unavailableButtonDoesNotActivate() {
            let button = createTemporaryObject(actionButtonComponent, root, { activated: false, available: false })
            verify(!!button, "Component exists")
            mouseClick(button)
            tryCompare(button, "activated", false)
        }

        // See tst_RecordSourceToggle: a disabled QML control receives no hover,
        // so the reason tooltip would be the one that could never appear.
        function test_unavailableButtonStillReportsHover() {
            let button = createTemporaryObject(actionButtonComponent, root, {
                                                   activated: false,
                                                   available: false,
                                                   unavailableReason: "Unavailable — the preview has not produced a frame yet."
                                               })
            verify(!!button, "Component exists")
            compare(button.hovered, false)
            mouseMove(button, button.width / 2, button.height / 2)
            tryCompare(button, "hovered", true)
            compare(button.Accessible.description,
                    "Unavailable — the preview has not produced a frame yet.")
        }
    }
}
