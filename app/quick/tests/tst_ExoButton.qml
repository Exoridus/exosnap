import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

Item {
    id: root

    width: 240
    height: 120

    Component {
        id: exoButtonComponent

        ExoButton {
            required property bool backendSelected

            width: 140
            height: 44
            selectable: true
            selected: backendSelected
            text: qsTr("Record")
            onClicked: backendSelected = true
        }
    }

    TestCase {
        name: "ExoButtonTests"
        when: windowShown

        function test_selectsFromAuthoritativeState() {
            let button = createTemporaryObject(exoButtonComponent, root, { backendSelected: false })
            verify(!!button, "Component exists")
            compare(button.selected, false)
            mouseClick(button)
            tryCompare(button, "backendSelected", true)
            tryCompare(button, "selected", true)
        }

        function test_currentSelectionCannotToggleOff() {
            let button = createTemporaryObject(exoButtonComponent, root, { backendSelected: true })
            verify(!!button, "Component exists")
            compare(button.selected, true)
            mouseClick(button)
            tryCompare(button, "backendSelected", true)
            tryCompare(button, "selected", true)
        }
    }
}
