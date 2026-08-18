import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

TestCase {
    id: testCase

    name: "ExoDisclosure"
    when: windowShown
    width: 320
    height: 160
    visible: true

    Component {
        id: disclosureComponent

        ExoDisclosure {
            title: "Evidence"
            subtitle: "Measured value and why it was recommended."

            body: Component {
                Rectangle {
                    objectName: "disclosureBody"
                    implicitHeight: 24
                }
            }
        }
    }

    function test_body_is_not_created_until_opened() {
        let disclosure = createTemporaryObject(disclosureComponent, testCase);
        verify(disclosure);

        // A collapsed section must cost nothing: the Loader stays inactive, so the
        // body object does not exist at all.
        compare(disclosure.expanded, false);
        compare(findBody(disclosure), null);

        disclosure.expanded = true;
        verify(findBody(disclosure) !== null);
    }

    function test_header_toggles_the_section() {
        let disclosure = createTemporaryObject(disclosureComponent, testCase);
        verify(disclosure);

        mouseClick(disclosure, 20, 14);
        compare(disclosure.expanded, true);
        mouseClick(disclosure, 20, 14);
        compare(disclosure.expanded, false);
    }

    function test_opening_grows_the_section() {
        let disclosure = createTemporaryObject(disclosureComponent, testCase);
        verify(disclosure);
        disclosure.width = 300;
        compare(disclosure.subtitle, "Measured value and why it was recommended.");

        let collapsedHeight = disclosure.implicitHeight;
        disclosure.expanded = true;
        // Subtitle and body only occupy space while open, so the section must be
        // strictly taller than its collapsed header.
        tryVerify(function () {
            return disclosure.implicitHeight > collapsedHeight;
        });
    }

    // Qt Quick's Accessible attached type has no expanded/collapsed property, so
    // a two-state button reports its state as checkable + checked. Without it a
    // screen reader announced "What is included in this report?, button" and
    // never said whether the section was open — the chevron that says so on
    // screen is a drawn shape.
    function test_header_reports_its_open_state_to_assistive_tools() {
        let disclosure = createTemporaryObject(disclosureComponent, testCase);
        verify(disclosure);
        let header = findHeader(disclosure);
        verify(header);

        compare(header.Accessible.role, Accessible.Button);
        compare(header.Accessible.name, "Evidence");
        compare(header.Accessible.checkable, true);
        compare(header.Accessible.checked, false);

        disclosure.expanded = true;
        compare(header.Accessible.checked, true);
    }

    function test_assistive_press_toggles_the_section() {
        let disclosure = createTemporaryObject(disclosureComponent, testCase);
        verify(disclosure);
        let header = findHeader(disclosure);
        verify(header);

        // The press action a screen reader invokes has to do what a click does.
        header.Accessible.pressAction();
        compare(disclosure.expanded, true);
        header.Accessible.pressAction();
        compare(disclosure.expanded, false);
    }

    // QCR-503. The header was a bare AbstractButton: it carried the Button role
    // asserted above while being unreachable by Tab and inert to Enter and
    // Space, so the section could only be opened with a pointer.
    function test_the_header_is_a_keyboard_target() {
        let disclosure = createTemporaryObject(disclosureComponent, testCase);
        verify(disclosure);
        let header = findHeader(disclosure);
        verify(header);

        compare(header.focusPolicy, Qt.StrongFocus);
        // Tab reason, not the default OtherFocusReason: `visualFocus` is Qt's
        // own "this focus came from the keyboard" flag, and it is what the ring
        // binds to — a header focused by a click must not sprout one.
        header.forceActiveFocus(Qt.TabFocusReason);
        verify(header.activeFocus);
        verify(header.visualFocus, "the focus ring binds to visualFocus");
    }

    // Space, which is Qt's own activation key for a focused button and the
    // Windows convention (Enter belongs to a dialog's default button, which a
    // section header is not). The frontend deliberately adds no second key.
    function test_space_toggles_the_focused_header() {
        let disclosure = createTemporaryObject(disclosureComponent, testCase);
        verify(disclosure);
        let header = findHeader(disclosure);
        verify(header);
        header.forceActiveFocus(Qt.TabFocusReason);

        keyClick(Qt.Key_Space);
        compare(disclosure.expanded, true);
        keyClick(Qt.Key_Space);
        compare(disclosure.expanded, false);
    }

    function findHeader(disclosure) {
        for (let i = 0; i < disclosure.children.length; ++i) {
            let child = disclosure.children[i];
            if (child.Accessible.role === Accessible.Button)
                return child;
        }
        return null;
    }

    function findBody(disclosure) {
        for (let i = 0; i < disclosure.children.length; ++i) {
            let child = disclosure.children[i];
            if (child.objectName === "disclosureBody") {
                return child;
            }
            for (let j = 0; j < child.children.length; ++j) {
                if (child.children[j].objectName === "disclosureBody") {
                    return child.children[j];
                }
            }
        }
        return null;
    }
}
