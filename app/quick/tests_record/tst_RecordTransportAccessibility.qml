pragma ComponentBehavior: Bound

import QtQuick
import QtTest

import ExoSnap.Quick.RecordPickerTestControls

// The transport's accessibility contract: the controls a user presses to start
// and pause a recording are exposed to assistive technology by their names,
// and pressing them reaches the transport adapter rather than stopping at QML.
TestCase {
    id: testCase

    name: "RecordTransportAccessibility"
    when: windowShown
    width: 1100
    height: 200
    visible: true

    Component {
        id: dockComponent

        RecordTransportDock {
            width: 1100
            height: 96
            recordViewModel: recordDriver.adapter
        }
    }

    SignalSpy {
        id: startSpy
        signalName: "startRequested"
    }

    SignalSpy {
        id: pauseSpy
        signalName: "pauseRequested"
    }

    function init() {
        startSpy.target = recordDriver.adapter;
        pauseSpy.target = recordDriver.adapter;
        startSpy.clear();
        pauseSpy.clear();
    }

    function cleanup() {
        recordDriver.setRecordingState("loading");
    }

    // Depth-first over visual children, the way an accessibility client walks
    // the tree: only a visible item can be found and pressed.
    function findAccessible(item, name) {
        if (!item || !item.visible)
            return null;
        if (item.Accessible.name === name)
            return item;
        for (let i = 0; i < item.children.length; ++i) {
            const found = findAccessible(item.children[i], name);
            if (found)
                return found;
        }
        return null;
    }

    function test_start_recording_is_named_and_reaches_the_adapter() {
        recordDriver.setRecordingState("ready");
        const dock = createTemporaryObject(dockComponent, testCase);
        verify(!!dock, "the transport dock exists");
        const start = findAccessible(dock, qsTr("Start recording"));
        verify(!!start, "a visible control is named Start recording");
        compare(start.Accessible.role, Accessible.Button);
        verify(start.enabled, "Start recording is enabled when a recording can start");
        mouseClick(start);
        compare(startSpy.count, 1);
    }

    function test_pause_is_named_and_reaches_the_adapter() {
        recordDriver.setRecordingState("recording");
        const dock = createTemporaryObject(dockComponent, testCase);
        verify(!!dock, "the transport dock exists");
        const pause = findAccessible(dock, qsTr("Pause recording"));
        verify(!!pause, "a visible control is named Pause recording while recording");
        mouseClick(pause);
        compare(pauseSpy.count, 1);
        compare(startSpy.count, 0);
    }

    function test_pause_is_absent_when_nothing_is_recording() {
        recordDriver.setRecordingState("ready");
        const dock = createTemporaryObject(dockComponent, testCase);
        verify(!!dock, "the transport dock exists");
        compare(findAccessible(dock, qsTr("Pause recording")), null);
    }

    function findClock(dock) {
        for (const child of dock.children) {
            if (child.objectName === "recordTransportClock")
                return child;
        }
        return null;
    }

    // A successful run returns the bar to its idle arrangement at once: Record
    // is offered again and nothing of the finished take stays on the bar.
    function test_completed_returns_to_the_idle_transport() {
        recordDriver.setRecordingState("completed");
        const dock = createTemporaryObject(dockComponent, testCase);
        verify(!!dock, "the transport dock exists");
        const start = findAccessible(dock, qsTr("Start recording"));
        verify(!!start, "Start recording is offered right after a successful recording");
        verify(start.enabled, "Start recording is enabled after a successful recording");
        compare(findAccessible(dock, qsTr("Back to the transport")), null);
        compare(findAccessible(dock, qsTr("Edit recording")), null);
        compare(findAccessible(dock, qsTr("Show the recording in Explorer")), null);
        compare(findClock(dock).text, "00:00:00");
        mouseClick(start);
        compare(startSpy.count, 1);
    }

    // A failure is not treated as a success: it keeps its acknowledgement and
    // withholds Record until the failure has been dismissed.
    function test_failed_keeps_its_acknowledgement() {
        recordDriver.setRecordingState("failed");
        const dock = createTemporaryObject(dockComponent, testCase);
        verify(!!dock, "the transport dock exists");
        verify(!!findAccessible(dock, qsTr("Back to the transport")), "a failed run offers Back");
        compare(findAccessible(dock, qsTr("Start recording")), null);
        compare(findClock(dock).text, "00:00:42");
    }

    function test_paused_clock_clears_actions_at_minimum_width() {
        recordDriver.setRecordingState("paused");
        const dock = createTemporaryObject(dockComponent, testCase);
        verify(!!dock);
        dock.width = 828;
        waitForRendering(dock);

        let clock = null;
        let actions = null;
        for (const child of dock.children) {
            if (child.objectName === "recordTransportClock")
                clock = child;
            if (child.objectName === "recordTransportActions")
                actions = child;
        }
        verify(clock && actions);
        compare(clock.text, "00:12:34");
        verify(clock.x + clock.width + 8 <= actions.x,
               "the paused clock must not overlap the action cluster");
    }
}
