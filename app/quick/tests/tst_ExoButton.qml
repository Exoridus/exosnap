import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

Item {
    id: root

    width: 240
    height: 120

    Component {
        id: quietButtonComponent

        ExoButton {
            width: 160
            height: 36
            quiet: true
            text: qsTr("Open crash folder")
        }
    }

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

        // A quiet button has no chrome at rest, so "disabled" has to be carried
        // by ink strength and by the absence of a hover response — the crash
        // surface's disabled "Open crash folder" was one ink step away from the
        // enabled one and nothing else.
        function test_disabledQuietActionIsClearlyWeakerAndDoesNotRespondToHover() {
            let enabledButton = createTemporaryObject(quietButtonComponent, root, { enabled: true })
            let disabledButton = createTemporaryObject(quietButtonComponent, root, { enabled: false })
            verify(!!enabledButton && !!disabledButton)

            // The disabled rung reaches the chrome only. Fading the whole
            // control takes `textDim` — which sits exactly ON the 3:1 floor for
            // an unavailable control — down to roughly 1.6:1, so the control's
            // own opacity has to stay at full strength.
            compare(enabledButton.opacity, 1.0)
            compare(disabledButton.opacity, 1.0)
            compare(enabledButton.background.opacity, 1.0)
            compare(disabledButton.background.opacity, ExoTheme.disabledOpacity)

            // Ink: one full token step down, at full alpha.
            compare(disabledButton.contentItem.children[0].color, ExoTheme.textDim)
            verify(enabledButton.contentItem.children[0].color !== ExoTheme.textDim)

            // No hover response at all: not merely a hover that looks the same.
            compare(disabledButton.hoverEnabled, false)
            mouseMove(disabledButton, disabledButton.width / 2, disabledButton.height / 2)
            compare(disabledButton.hovered, false)

            // The accessible "disabled" state needs nothing added here: Qt maps
            // it from `enabled` on the item itself, so the only way to get it
            // wrong would be to fake the look while leaving the control live.
            compare(disabledButton.enabled, false)
        }
    }
}
