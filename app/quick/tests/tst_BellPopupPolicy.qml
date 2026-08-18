import QtQuick
import QtQuick.Controls.Basic
import QtTest

import ExoSnap.Quick.TestControls

// QCR-410. The close policy a popup anchored to its own toggle button may use.
//
// NotificationHub is a Popup parented to NotificationBell, and the bell's tap
// calls NotificationsAdapter.toggleHub(). Neither of those two can be built
// here — both require the application-provided adapter — so this reproduces the
// exact structure they form: a toggle item that flips one boolean, and a popup
// whose `visible` mirrors it and whose `onClosed` clears it.
//
// What it pins down is which closePolicy survives a click on the toggle while
// the popup is open, because that click is delivered twice: once to the overlay
// as a press outside the popup, and once to the toggle as a release.
TestCase {
    id: testCase

    name: "BellPopupPolicy"
    when: windowShown
    width: 400
    height: 300
    visible: true

    Component {
        id: bellComponent

        Item {
            id: fixture

            // Stands in for NotificationsAdapter.hubOpen: the one boolean both
            // the bell and the hub agree on.
            property bool hubOpen: false
            property alias popup: hubPopup
            property int closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

            function toggleHub() {
                fixture.hubOpen = !fixture.hubOpen;
            }

            width: 400
            height: 300

            Rectangle {
                id: bell

                x: 300
                y: 4
                width: 32
                height: 32
                color: "transparent"

                TapHandler {
                    onTapped: fixture.toggleHub()
                }

                Popup {
                    id: hubPopup

                    parent: bell
                    y: bell.height + 4
                    x: bell.width - width
                    width: 200
                    height: 160
                    modal: false
                    focus: true
                    closePolicy: fixture.closePolicy
                    visible: fixture.hubOpen

                    onClosed: fixture.hubOpen = false
                }
            }
        }
    }

    function test_clicking_the_bell_opens_and_closes_the_hub() {
        let fixture = createTemporaryObject(bellComponent, testCase);
        verify(fixture);

        mouseClick(fixture, 316, 20);
        tryCompare(fixture, "hubOpen", true);

        // The click that must close it again. With CloseOnPressOutside the
        // press closed the popup and the release re-opened it, so the hub was
        // unclosable by the control that opened it.
        mouseClick(fixture, 316, 20);
        tryCompare(fixture, "hubOpen", false);

        // Second cycle: whatever the first click left behind must not make the
        // next one behave differently.
        mouseClick(fixture, 316, 20);
        tryCompare(fixture, "hubOpen", true);
        mouseClick(fixture, 316, 20);
        tryCompare(fixture, "hubOpen", false);
    }

    function test_pressing_elsewhere_still_closes_the_hub() {
        let fixture = createTemporaryObject(bellComponent, testCase);
        verify(fixture);

        fixture.hubOpen = true;
        tryCompare(fixture.popup, "opened", true);

        mouseClick(fixture, 20, 200);
        tryCompare(fixture, "hubOpen", false);
    }

    function test_escape_closes_the_hub() {
        let fixture = createTemporaryObject(bellComponent, testCase);
        verify(fixture);

        fixture.hubOpen = true;
        tryCompare(fixture.popup, "opened", true);

        keyClick(Qt.Key_Escape);
        tryCompare(fixture, "hubOpen", false);
    }

    // The defect itself, kept as a test so the policy is never "simplified"
    // back to CloseOnPressOutside on the theory that the two are equivalent.
    function test_close_on_press_outside_reopens_on_the_toggle() {
        let fixture = createTemporaryObject(bellComponent, testCase);
        verify(fixture);
        fixture.closePolicy = Popup.CloseOnEscape | Popup.CloseOnPressOutside;

        mouseClick(fixture, 316, 20);
        tryCompare(fixture, "hubOpen", true);

        mouseClick(fixture, 316, 20);
        // Still open: the press closed it, the release toggled it back on.
        compare(fixture.hubOpen, true);
    }
}
