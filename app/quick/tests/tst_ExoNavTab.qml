import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// The title band's destinations. The label goes bold on selection, so the tab's
// width is the thing that must NOT follow it: a row whose cells resize when the
// selection moves shifts every neighbouring destination under the pointer.
TestCase {
    id: testCase

    name: "ExoNavTab"
    when: windowShown
    width: 480
    height: 120
    visible: true

    Component {
        id: tabComponent

        ExoNavTab {
            text: "Settings"
        }
    }

    TextMetrics {
        id: demiBoldMetrics

        text: "Settings"
        font.family: ExoTheme.sansFamily
        font.pixelSize: ExoTheme.fontBody
        font.weight: Font.DemiBold
    }

    function test_selection_does_not_change_the_tab_width() {
        let tab = createTemporaryObject(tabComponent, testCase);
        verify(tab);

        tab.selected = false;
        const unselected = tab.implicitWidth;
        tab.selected = true;
        const selected = tab.implicitWidth;

        compare(selected, unselected, "selected " + selected + " vs unselected " + unselected);
    }

    function test_the_tab_is_sized_for_the_bold_label() {
        // Sizing from the lighter weight would fit the selected label only by
        // eliding it, which is the same defect seen from the other side.
        let tab = createTemporaryObject(tabComponent, testCase);
        verify(tab);
        verify(tab.implicitWidth >= demiBoldMetrics.advanceWidth + tab.leftPadding + tab.rightPadding,
               "implicitWidth was " + tab.implicitWidth);
    }
}
