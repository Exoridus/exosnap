import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// The shortcut field captures key combinations instead of text. What matters is
// input behaviour: which keys it keeps, which it lets through for focus
// navigation, and where focus goes once a combination is complete.
TestCase {
    id: testCase

    name: "HotkeyCaptureField"
    when: windowShown
    width: 400
    height: 160
    visible: true

    Component {
        id: fieldComponent

        Item {
            property alias field: field
            property alias before: before
            property alias after: after
            property var lastCapture: null
            property int captureStarts: 0

            width: 400
            height: 120

            TextInput {
                id: before

                width: 100
                height: 20
                activeFocusOnTab: true
            }

            HotkeyCaptureField {
                id: field

                y: 30
                width: 240
                keys: ["Alt", "Shift", "R"]
                bindingText: "Alt+Shift+R"
                modifierNames: ["Ctrl", "Alt", "Shift", "Win"]
                clearAccessibleName: "Clear"
                onCaptureStarted: parent.captureStarts += 1
                onCaptured: (key, modifiers) => parent.lastCapture = { key: key, modifiers: modifiers }
            }

            TextInput {
                id: after

                y: 80
                width: 100
                height: 20
                activeFocusOnTab: true
            }
        }
    }

    function capLabels(field) {
        return field.shownKeys.slice();
    }

    function test_focus_starts_the_capture_and_shows_held_modifiers() {
        let host = createTemporaryObject(fieldComponent, testCase);
        verify(host);
        host.field.forceActiveFocus();
        compare(host.captureStarts, 1);
        verify(host.field.capturing);
        compare(capLabels(host.field), [], "an empty capture shows the prompt, not the old binding");

        keyPress(Qt.Key_Control, Qt.ControlModifier);
        keyPress(Qt.Key_Shift, Qt.ControlModifier | Qt.ShiftModifier);
        compare(capLabels(host.field), ["Ctrl", "Shift"]);
        // Released without a modifier argument: QtTest releases every listed
        // modifier along with the key, which a real keyboard does not.
        keyRelease(Qt.Key_Shift, Qt.NoModifier);
        compare(capLabels(host.field), ["Ctrl"]);
        keyRelease(Qt.Key_Control, Qt.NoModifier);
        compare(capLabels(host.field), []);
    }

    function test_a_combination_is_reported_and_focus_moves_to_clear() {
        let host = createTemporaryObject(fieldComponent, testCase);
        verify(host);
        host.field.forceActiveFocus();

        keyClick(Qt.Key_K, Qt.ControlModifier | Qt.ShiftModifier);
        verify(host.lastCapture !== null, "the combination is reported");
        compare(host.lastCapture.key, Qt.Key_K);
        compare(host.lastCapture.modifiers, Qt.ControlModifier | Qt.ShiftModifier);
        verify(host.field.clearButton.activeFocus, "the clear button is the next step");
        verify(!host.field.capturing);
    }

    function test_typed_text_never_lands_and_keypad_is_not_a_modifier() {
        let host = createTemporaryObject(fieldComponent, testCase);
        verify(host);
        host.field.forceActiveFocus();

        keyClick(Qt.Key_5, Qt.KeypadModifier | Qt.AltModifier);
        compare(host.lastCapture.modifiers, Qt.AltModifier);
    }

    function test_tab_and_shift_tab_still_move_focus() {
        let host = createTemporaryObject(fieldComponent, testCase);
        verify(host);
        host.field.forceActiveFocus();

        keyClick(Qt.Key_Tab);
        compare(host.lastCapture, null, "Tab is navigation, not a shortcut");
        verify(!host.field.activeFocus);

        host.field.forceActiveFocus();
        keyClick(Qt.Key_Backtab, Qt.ShiftModifier);
        compare(host.lastCapture, null);
        verify(host.before.activeFocus, "Shift+Tab goes back");
    }

    function test_ctrl_tab_is_a_combination() {
        let host = createTemporaryObject(fieldComponent, testCase);
        verify(host);
        host.field.forceActiveFocus();

        keyClick(Qt.Key_Tab, Qt.ControlModifier);
        verify(host.lastCapture !== null);
        compare(host.lastCapture.key, Qt.Key_Tab);
    }

    function test_escape_drops_held_modifiers_without_capturing() {
        let host = createTemporaryObject(fieldComponent, testCase);
        verify(host);
        host.field.forceActiveFocus();

        keyPress(Qt.Key_Alt, Qt.AltModifier);
        keyRelease(Qt.Key_Alt, Qt.NoModifier);
        keyClick(Qt.Key_Escape);
        compare(host.lastCapture, null);
        compare(capLabels(host.field), []);
        verify(host.field.capturing, "Escape does not leave the field");
    }

    function test_an_error_shows_the_refused_attempt_and_clear_dismisses_it() {
        let host = createTemporaryObject(fieldComponent, testCase, {});
        verify(host);
        let dismissed = 0;
        host.field.errorDismissed.connect(function () { dismissed += 1; });
        host.field.errorText = "Already assigned to Pause.";
        host.field.errorKeys = ["Alt", "Shift", "P"];

        compare(capLabels(host.field), ["Alt", "Shift", "P"]);
        compare(host.field.Accessible.description, "Already assigned to Pause.");
        verify(host.field.clearButton.visible);

        mouseClick(host.field.clearButton);
        compare(dismissed, 1);
        verify(host.field.activeFocus, "the field is ready for the next combination");
    }

    function test_clear_is_hidden_while_nothing_is_bound() {
        let host = createTemporaryObject(fieldComponent, testCase);
        verify(host);
        host.field.keys = [];
        verify(!host.field.clearButton.visible);
    }
}
