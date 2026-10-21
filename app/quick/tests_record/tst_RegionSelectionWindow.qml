pragma ComponentBehavior: Bound

import QtQuick
import QtTest
import ExoSnap.Quick.RecordPickerTestControls

TestCase {
    name: "RegionSelectionWindowSafety"
    when: windowShown

    Component {
        id: selectorComponent

        RegionSelectionWindow {
            recordViewModel: recordDriver.adapter
            monitorGeometry: Qt.rect(0, 0, 800, 600)
        }
    }

    function test_normal_offscreen_cannot_show_unexcluded_selector() {
        recordDriver.prepareRegionSelection();
        verify(recordDriver.adapter.regionSelectionNeeded);
        const selector = createTemporaryObject(selectorComponent, null);
        verify(selector !== null);
        verify(!selector.visible, "A requested region cannot bypass capture exclusion.");
    }

    function test_normal_offscreen_refuses_visual_draft_hook() {
        recordDriver.prepareRegionSelection();
        const selector = createTemporaryObject(selectorComponent, null);
        verify(selector !== null);
        compare(typeof selector.prepareVisualSelection, "function");
        verify(!selector.prepareVisualSelection(Qt.rect(0.15, 0.18, 0.6, 0.6)));
        verify(!selector.visible);
    }
}
