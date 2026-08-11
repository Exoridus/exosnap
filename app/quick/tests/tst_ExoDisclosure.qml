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
