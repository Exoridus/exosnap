import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

Item {
    id: root

    width: 160
    height: 100

    Component {
        id: sourceToggleComponent

        RecordSourceToggle {
            required property bool activated

            width: 64
            height: 44
            shortLabel: qsTr("MIC")
            accessibleLabel: qsTr("Microphone")
            onClicked: activated = true
        }
    }

    TestCase {
        name: "RecordSourceToggleTests"
        when: windowShown

        function test_presentationProperties() {
            let toggle = createTemporaryObject(sourceToggleComponent, root, { activated: false })
            verify(!!toggle, "Component exists")
            compare(toggle.text, qsTr("MIC"))
            compare(toggle.Accessible.name, qsTr("Microphone"))
            compare(toggle.checkedState, false)
            compare(toggle.errorState, false)
            compare(toggle.meterLevel, 0)
        }

        function test_stateInputs() {
            let toggle = createTemporaryObject(sourceToggleComponent, root, {
                                                   activated: false,
                                                   checkedState: true,
                                                   errorState: true,
                                                   meterLevel: 0.75
                                               })
            verify(!!toggle, "Component exists")
            compare(toggle.checkedState, true)
            compare(toggle.errorState, true)
            compare(toggle.meterLevel, 0.75)
        }

        function test_mouseActivation() {
            let toggle = createTemporaryObject(sourceToggleComponent, root, { activated: false })
            verify(!!toggle, "Component exists")
            mouseClick(toggle)
            tryCompare(toggle, "activated", true)
        }

        function test_keyboardActivation() {
            let toggle = createTemporaryObject(sourceToggleComponent, root, { activated: false })
            verify(!!toggle, "Component exists")
            toggle.forceActiveFocus()
            keyClick(Qt.Key_Space)
            tryCompare(toggle, "activated", true)
        }

        function test_unavailableToggleDoesNotActivate() {
            let toggle = createTemporaryObject(sourceToggleComponent, root, { activated: false, available: false })
            verify(!!toggle, "Component exists")
            mouseClick(toggle)
            tryCompare(toggle, "activated", false)
        }

        // The reason this control cannot be used is exactly the tooltip a
        // disabled QML control cannot show, because a disabled item receives no
        // hover at all. The scope around the button is what keeps it reachable,
        // so hover on an unavailable toggle is the property under test.
        function test_unavailableToggleStillReportsHover() {
            let toggle = createTemporaryObject(sourceToggleComponent, root, {
                                                   activated: false,
                                                   available: false,
                                                   unavailableReason: "Unavailable — no microphone was detected."
                                               })
            verify(!!toggle, "Component exists")
            compare(toggle.hovered, false)
            mouseMove(toggle, toggle.width / 2, toggle.height / 2)
            tryCompare(toggle, "hovered", true)
            compare(toggle.Accessible.description, "Unavailable — no microphone was detected.")
        }
    }
}
