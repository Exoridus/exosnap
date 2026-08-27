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

        // The Record button is the one the developer noticed: its scope stays
        // enabled while the action is unavailable so it can still speak its
        // reason, so the cursor is the only thing left to say it cannot be
        // pressed.
        // Recursive: the handler lives on a transparent surface in front of the
        // Button, not directly on the scope. A Controls item that fills its parent
        // swallows the parent's hover, so a handler declared on the scope asks for
        // a cursor the desktop never shows -- see test_hover_cursor_native.cpp.
        function cursorShapeOf(item) {
            if (!item || !item.data)
                return -1;
            for (let i = 0; i < item.data.length; ++i) {
                const child = item.data[i];
                if (!child)
                    continue;
                if (child.cursorShape !== undefined)
                    return child.cursorShape;
                const nested = cursorShapeOf(child);
                if (nested !== -1)
                    return nested;
            }
            return -1;
        }

        function test_the_cursor_follows_availability() {
            let button = createTemporaryObject(actionButtonComponent, root, { activated: false })
            verify(!!button, "Component exists")
            compare(cursorShapeOf(button), Qt.PointingHandCursor)

            button.available = false
            compare(cursorShapeOf(button), Qt.ArrowCursor)
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
