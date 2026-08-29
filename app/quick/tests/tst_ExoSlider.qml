import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// Can the keyboard reach the slider at all?
//
// ExoSlider paints a focus ring off `visualFocus`, and it is the only shared
// control that sets neither a focusPolicy nor activeFocusOnTab. Qt Quick Controls
// default activeFocusOnTab to false, so a ring it can never show and a control
// no Tab press ever lands on look identical from the file -- which is why this is
// measured by pressing Tab rather than by reading the property.
TestCase {
    id: testCase

    name: "ExoSlider"
    when: windowShown
    width: 320
    height: 200
    visible: true

    Component {
        id: rowComponent

        Column {
            property alias first: firstSwitch
            property alias slider: middleSlider
            property alias last: lastSwitch

            ExoSwitch { id: firstSwitch }
            ExoSlider { id: middleSlider; from: 0; to: 100; value: 50 }
            ExoSwitch { id: lastSwitch }
        }
    }

    function test_tab_reaches_the_slider_between_its_neighbours() {
        let row = createTemporaryObject(rowComponent, testCase);
        verify(row);

        row.first.forceActiveFocus();
        tryCompare(row.first, "activeFocus", true);

        keyClick(Qt.Key_Tab);
        tryCompare(row.slider, "activeFocus", true,
                   2000, "Tab skipped the slider: it is focusable by mouse only");

        keyClick(Qt.Key_Tab);
        tryCompare(row.last, "activeFocus", true);
    }

    // Reachable is not the same as usable. A focused slider has to be movable
    // without a pointer, which is the whole reason it is in the tab ring.
    function test_the_arrow_keys_move_a_focused_slider() {
        let row = createTemporaryObject(rowComponent, testCase);
        verify(row);

        row.slider.forceActiveFocus();
        tryCompare(row.slider, "activeFocus", true);

        const before = row.slider.value;
        keyClick(Qt.Key_Right);
        verify(row.slider.value > before, "Right did not move a focused slider");
    }
}
