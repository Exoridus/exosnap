import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// The pill is the one capture-excluded overlay that is not click-through, so a
// part of it sitting under the taskbar is an unclickable control, not merely a
// cosmetic overlap. Before this test, the default position and the drag clamp
// were computed against the full monitor rectangle
// (OverlayAdapter::recordedMonitorGeometry), which includes the taskbar; a
// monitor with a taller taskbar than the hard-coded 32 px margin left the
// bottom of the pill behind it.
//
// The window is never shown here: `visible` is gated on CaptureExclusion.granted,
// which stays false without a real platform window, and every assertion below
// is about the geometry bindings, not about anything on screen.
TestCase {
    id: testCase

    name: "OverlayQuickControlPill"
    when: windowShown
    width: 400
    height: 200
    visible: true

    Component {
        id: pillComponent

        OverlayQuickControlPill {
            overlayActive: true
        }
    }

    // A monitor whose taskbar takes 96 px off the bottom -- taller than the
    // pill's 32 px default margin, so a bug that measures from the full
    // rectangle instead of the work area is caught rather than hidden by a
    // taskbar that happens to be thinner than the margin.
    readonly property rect workArea: Qt.rect(100, 50, 1600, 804)
    readonly property rect fullMonitor: Qt.rect(100, 50, 1600, 900)

    function test_default_position_keeps_the_pill_inside_the_work_area() {
        let pill = createTemporaryObject(pillComponent, testCase, {
            workAreaGeometry: testCase.workArea
        });
        verify(pill);

        const bottom = pill.y + pill.height;
        verify(bottom <= testCase.workArea.y + testCase.workArea.height,
               "pill bottom " + bottom + " must stay above the work area's bottom edge "
               + (testCase.workArea.y + testCase.workArea.height));
        // The same edge measured against the full monitor rectangle would sit
        // 96 px lower, inside the taskbar -- this is the regression itself.
        verify(bottom < testCase.fullMonitor.y + testCase.fullMonitor.height - 32);
    }

    function test_default_position_is_bottom_centred_on_the_work_area() {
        let pill = createTemporaryObject(pillComponent, testCase, {
            workAreaGeometry: testCase.workArea
        });
        verify(pill);

        compare(pill.x, testCase.workArea.x + (testCase.workArea.width - pill.width) / 2);
        compare(pill.y, testCase.workArea.y + testCase.workArea.height - pill.height - 32);
    }

    // No monitor bound (an unresolved or non-monitor capture target) falls back
    // to the same full-screen rectangle the other overlays use -- unchanged by
    // this fix, and worth pinning down because it is the one path with no
    // C++-supplied rectangle to assert against.
    function test_an_empty_work_area_falls_back_to_the_screen() {
        let pill = createTemporaryObject(pillComponent, testCase, {
            workAreaGeometry: Qt.rect(0, 0, 0, 0)
        });
        verify(pill);

        compare(pill.effectiveWorkArea, Qt.rect(Screen.virtualX, Screen.virtualY, Screen.width, Screen.height));
    }
}
