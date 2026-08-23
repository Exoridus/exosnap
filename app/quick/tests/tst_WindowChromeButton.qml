import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// The Maximize button is the one window button Qt never sees the pointer on.
// Its rect answers WM_NCHITTEST with HTMAXBUTTON so Windows 11 can offer the
// Snap Layouts flyout, and that reclassifies the rect as non-client: no mouse
// event is delivered there, so `hovered` stays false for as long as the pointer
// sits on it. The chrome rebuilds the state from the non-client message stream
// and pushes it in.
//
// Shipped once with the push side missing entirely: the chrome tracked hover,
// press and the click, and no QML read any of them. Minimize and Close kept
// their highlight, Maximize had none, and nothing failed.
TestCase {
    id: testCase

    name: "WindowChromeButton"
    when: windowShown
    width: 200
    height: 80
    visible: true

    Component {
        id: buttonComponent

        WindowChromeButton {
            kind: "maximize"
        }
    }

    function test_a_client_side_button_highlights_from_qt_hover() {
        let button = createTemporaryObject(buttonComponent, testCase);
        verify(button);

        compare(button.highlighted, false);
        // Qt normalises an unset background to fully transparent black, so the
        // resting state is asserted on the alpha channel rather than on a colour
        // name -- the RGB channels are zero there and carry no information.
        compare(button.background.color.a, 0);
    }

    function test_non_client_hover_highlights_the_button() {
        let button = createTemporaryObject(buttonComponent, testCase);
        verify(button);

        button.nonClientHovered = true;

        compare(button.highlighted, true);
        // The visible contract, not just the flag: an unhighlighted button is
        // transparent, so a highlight that does not reach the background is the
        // defect this pins.
        compare(button.background.color, ExoTheme.surfaceHover);
    }

    // The press arrives as its own non-client message and outlives the hover
    // state on a pointer that leaves the rect while the button is held.
    function test_non_client_press_highlights_the_button() {
        let button = createTemporaryObject(buttonComponent, testCase);
        verify(button);

        button.nonClientPressed = true;

        compare(button.highlighted, true);
        compare(button.background.color, ExoTheme.surfaceHover);
    }

    function test_clearing_the_non_client_state_restores_the_resting_button() {
        let button = createTemporaryObject(buttonComponent, testCase);
        verify(button);

        button.nonClientHovered = true;
        button.nonClientPressed = true;
        compare(button.highlighted, true);

        button.nonClientHovered = false;
        button.nonClientPressed = false;

        compare(button.highlighted, false);
        compare(button.background.color.a, 0);
    }

    // Close is the one button that answers a highlight with a full colour rather
    // than a surface tint, and the non-client route must not bypass that.
    function test_the_danger_variant_keeps_its_own_highlight_colour() {
        let button = createTemporaryObject(buttonComponent, testCase);
        verify(button);
        button.kind = "close";
        button.danger = true;

        button.nonClientHovered = true;

        compare(button.background.color, ExoTheme.error);
    }
}
