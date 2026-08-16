import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

TestCase {
    id: testCase

    name: "ExoSegmentedControl"
    when: windowShown
    width: 320
    height: 60
    visible: true

    Component {
        id: segmentedComponent

        ExoSegmentedControl {
            options: ["All", "Info", "Issues"]
        }
    }

    function test_reports_the_tapped_segment_index() {
        let control = createTemporaryObject(segmentedComponent, testCase);
        verify(control);

        let spy = createTemporaryObject(signalSpyComponent, testCase, {
            target: control,
            signalName: "selected"
        });
        verify(spy);

        // Second segment: 3px inset + one segment width in.
        let segmentWidth = control.width / 3;
        mouseClick(control, segmentWidth + segmentWidth / 2, control.height / 2);
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], 1);
    }

    function test_current_index_is_owned_by_the_caller() {
        let control = createTemporaryObject(segmentedComponent, testCase);
        verify(control);

        // The control is stateless: tapping does not move the selection by itself,
        // so the adapter stays the single source of truth for the active filter.
        compare(control.currentIndex, 0);
        mouseClick(control, control.width - 10, control.height / 2);
        compare(control.currentIndex, 0);

        control.currentIndex = 2;
        compare(control.currentIndex, 2);
    }

    // QCR-503. The segments were AbstractButtons with no focus policy, so the
    // whole control was mouse-only. It is a radio group: ONE tab stop, arrows
    // within it.
    function test_the_group_is_one_tab_stop() {
        let control = createTemporaryObject(segmentedComponent, testCase);
        verify(control);
        compare(control.activeFocusOnTab, true);
        control.forceActiveFocus();
        verify(control.activeFocus);
    }

    function test_arrows_move_the_selection_and_wrap() {
        let control = createTemporaryObject(segmentedComponent, testCase);
        verify(control);
        control.forceActiveFocus();

        let spy = createTemporaryObject(signalSpyComponent, testCase, {
            target: control,
            signalName: "selected"
        });
        verify(spy);

        // The control stays stateless — it reports, the caller decides — so the
        // test plays the caller and writes back what it is told.
        control.selected.connect(function (index) { control.currentIndex = index; });

        keyClick(Qt.Key_Right);
        compare(control.currentIndex, 1);
        keyClick(Qt.Key_Down);
        compare(control.currentIndex, 2);
        keyClick(Qt.Key_Right);
        compare(control.currentIndex, 0, "the last segment wraps to the first");
        keyClick(Qt.Key_Left);
        compare(control.currentIndex, 2, "and back the other way");
        keyClick(Qt.Key_Up);
        compare(control.currentIndex, 1);
        compare(spy.count, 5);
    }

    function test_home_and_end_reach_the_ends() {
        let control = createTemporaryObject(segmentedComponent, testCase);
        verify(control);
        control.forceActiveFocus();
        control.selected.connect(function (index) { control.currentIndex = index; });

        keyClick(Qt.Key_End);
        compare(control.currentIndex, 2);
        keyClick(Qt.Key_Home);
        compare(control.currentIndex, 0);
    }

    function test_a_key_at_an_end_that_changes_nothing_reports_nothing() {
        let control = createTemporaryObject(segmentedComponent, testCase);
        verify(control);
        control.forceActiveFocus();

        let spy = createTemporaryObject(signalSpyComponent, testCase, {
            target: control,
            signalName: "selected"
        });
        verify(spy);

        keyClick(Qt.Key_Home);
        compare(spy.count, 0, "already at index 0");
    }

    Component {
        id: signalSpyComponent

        SignalSpy {}
    }
}
