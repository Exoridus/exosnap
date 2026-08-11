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

    Component {
        id: signalSpyComponent

        SignalSpy {}
    }
}
