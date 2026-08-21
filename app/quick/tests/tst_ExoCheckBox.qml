import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// QCR-506: the interaction target, and the keyboard contract that has to
// survive the padding that produced it.
TestCase {
    id: testCase

    name: "ExoCheckBox"
    when: windowShown
    width: 320
    height: 120
    visible: true

    Component {
        id: labelledComponent

        ExoCheckBox {
            text: "Mix into previous track"
        }
    }

    Component {
        id: bareComponent

        ExoCheckBox {}
    }

    function test_the_interaction_target_is_at_least_24px() {
        let box = createTemporaryObject(labelledComponent, testCase);
        verify(box);
        verify(box.implicitHeight >= 24, "implicitHeight was " + box.implicitHeight);
        verify(box.height >= 24, "height was " + box.height);
    }

    function test_a_labelless_checkbox_still_reaches_24px_in_both_axes() {
        // No shipped call site is labelless today; the floor is what keeps one
        // that appears later from shipping an 18 px target.
        let box = createTemporaryObject(bareComponent, testCase);
        verify(box);
        verify(box.implicitWidth >= 24, "implicitWidth was " + box.implicitWidth);
        verify(box.implicitHeight >= 24, "implicitHeight was " + box.implicitHeight);
    }

    function test_the_drawn_indicator_did_not_grow() {
        // The point of the padding is a bigger TARGET, not a bigger box: the
        // design's 18 px indicator is unchanged, and it stays centred in the
        // taller control.
        let box = createTemporaryObject(labelledComponent, testCase);
        verify(box);
        compare(box.indicator.width, 18);
        compare(box.indicator.height, 18);
        compare(box.indicator.y + box.indicator.height / 2, box.height / 2);
    }

    function test_space_toggles_a_focused_checkbox() {
        let box = createTemporaryObject(labelledComponent, testCase);
        verify(box);
        box.forceActiveFocus();
        verify(box.activeFocus);
        compare(box.checked, false);

        keyClick(Qt.Key_Space);
        compare(box.checked, true);
        keyClick(Qt.Key_Space);
        compare(box.checked, false);
    }

    function test_a_click_anywhere_on_the_row_toggles_it() {
        // The label is part of the target, which is what makes the width floor
        // above irrelevant in practice.
        let box = createTemporaryObject(labelledComponent, testCase);
        verify(box);
        compare(box.checked, false);
        mouseClick(box, box.width - 4, box.height / 2);
        compare(box.checked, true);
    }
}
