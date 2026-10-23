import QtQuick
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: testCase
    name: "EditLoopState"
    when: windowShown
    width: 640
    height: 240
    visible: true

    Component {
        id: timelineComponent
        EditTimeline {
            session: testLoopSession
            player: testLoopPlayer
            width: 600
            height: 200
        }
    }

    function init() {
        if (testLoopRealMedia)
            tryVerify(() => !testLoopSession.importPending && testLoopSession.durationUs >= 6000000);
        testLoopSession.cancelRangeGesture();
        testLoopSession.cancelClipDrag();
        testLoopPlayer.setPlaying(false);
        testLoopPlayer.loopEnabled = false;
        testLoopSession.clearEditRange();
        testLoopSession.requestSeekUs(0);
    }

    function setRange() {
        verify(testLoopSession.setEditRangeUs(1000000, 2000000, [testLoopSession.tracks[0].id]));
    }

    function test_loop_latches_without_seeking_and_clip_selection_keeps_it() {
        setRange();
        testLoopSession.requestSeekUs(5000000);
        testLoopPlayer.loopEnabled = true;
        compare(testLoopSession.positionUs, 5000000);
        compare(testLoopPlayer.desiredPlaying, false);
        compare(testLoopPlayer.loopRegion.startUs, 1000000);
        testLoopSession.selectClip(testLoopSession.visibleClipsUs(0, testLoopSession.durationUs)[0].id);
        compare(testLoopSession.selectionMode, 1);
        compare(testLoopPlayer.loopRegion.endUs, 2000000);
        testLoopSession.clearEditRange();
        compare(testLoopPlayer.loopEnabled, true);
        compare(testLoopPlayer.loopRegion.startUs, undefined);
    }

    function test_range_once_keeps_its_run_and_outside_seek_exits_both_modes() {
        setRange();
        testLoopPlayer.loopEnabled = true;
        testLoopPlayer.playRangeOnce();
        compare(testLoopPlayer.rangeOnceActive, true);
        compare(testLoopSession.positionUs, 1000000);
        testLoopPlayer.setPlaying(false);
        verify(testLoopSession.setEditRangeUs(4000000, 6000000, [testLoopSession.tracks[0].id]));
        compare(testLoopPlayer.rangeOnceActive, true);
        compare(testLoopPlayer.loopRegion.startUs, 4000000);
        testLoopSession.requestSeekUs(1500000);
        compare(testLoopPlayer.rangeOnceActive, true);
        testLoopSession.requestSeekUs(3000000);
        compare(testLoopPlayer.rangeOnceActive, false);
        compare(testLoopPlayer.loopEnabled, false);
    }

    function test_pause_cancels_range_candidate_without_reverting_newer_loop_command() {
        setRange();
        testLoopPlayer.loopEnabled = true;
        verify(testLoopSession.beginRangeGestureUs(1000000, [testLoopSession.tracks[0].id]));
        testLoopSession.updateRangeGestureUs(5000000);
        compare(testLoopPlayer.loopRegion.endUs, 2000000);
        testLoopPlayer.setPlaying(false);
        compare(testLoopSession.rangeGestureActive, false);
        testLoopPlayer.loopEnabled = false;
        testLoopSession.cancelRangeGesture();
        compare(testLoopPlayer.loopEnabled, false);
        compare(testLoopSession.editRange.endUs, 2000000);
    }

    function test_range_scope_change_does_not_seek_the_repeating_output() {
        setRange();
        verify(testLoopSession.tracks.length > 1);
        testLoopPlayer.loopEnabled = true;
        testLoopSession.requestSeekUs(1500000);
        testLoopPlayer.setPlaying(true);
        const before = testEditHarness.transportCommand(testLoopPlayer);
        verify(testLoopSession.setEditRangeUs(1000000, 2000000, [testLoopSession.tracks[1].id]));
        compare(testLoopPlayer.loopRegion.startUs, 1000000);
        compare(testLoopPlayer.loopRegion.endUs, 2000000);
        compare(testEditHarness.transportCommand(testLoopPlayer), before);
        compare(testLoopSession.positionUs, 1500000);
    }

    function test_shift_space_starts_once_on_press_and_never_toggles_on_release() {
        if (!testLoopRealMedia)
            skip("Real media is required to separate keyboard dispatch from asynchronous source-open failure.");
        setRange();
        const strip = createTemporaryObject(timelineComponent, testCase);
        verify(strip);
        strip.forceActiveFocus();
        const before = testEditHarness.transportCommand(testLoopPlayer);
        keyPress(Qt.Key_Space, Qt.ShiftModifier);
        compare(testLoopPlayer.rangeOnceActive, true);
        const pressed = testEditHarness.transportCommand(testLoopPlayer);
        verify(pressed > before);
        keyRelease(Qt.Key_Space, Qt.ShiftModifier);
        compare(testEditHarness.transportCommand(testLoopPlayer), pressed);
    }
}
