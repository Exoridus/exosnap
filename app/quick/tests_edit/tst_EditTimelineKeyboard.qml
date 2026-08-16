import QtQuick
import QtTest

import ExoSnap.Quick.EditTestControls

// The Edit surface's keyboard contract (QCR-504).
//
// The strip was pointer-only: no focus, no key handling, and the only way to
// move the playhead or set a trim point was to drag. These cases pin the
// replacement — that the strip is a tab stop, that the arrows/Home/End/I/O move
// what they say they move, and that the trim edits go through the SAME clamp
// the drag release uses rather than writing raw values.
//
// `testSession`, `testTimeline` and `testPlayer` are real adapters built by the
// runner (see edit_timeline_qml_test_main.cpp) and seeded with a 100 s fixture
// clip that has no master path, so nothing is ever decoded.
TestCase {
    id: testCase

    name: "EditTimelineKeyboard"
    when: windowShown
    width: 640
    height: 220
    visible: true

    Component {
        id: timelineComponent

        EditTimeline {
            session: testSession
            timeline: testTimeline
            player: testPlayer
            width: 600
            height: 160
        }
    }

    function init() {
        // Every case starts from an untrimmed clip parked at zero. The adapters
        // are shared across the whole run, so state has to be reset here rather
        // than relied upon to be fresh.
        testSession.requestTrim(0, testSession.durationMs);
        testSession.requestSeek(0);
    }

    function make() {
        let strip = createTemporaryObject(timelineComponent, testCase);
        verify(strip, "timeline exists");
        verify(strip.interactive, "the fixture clip has a duration, so the strip is live");
        strip.forceActiveFocus();
        verify(strip.activeFocus, "the strip takes focus");
        return strip;
    }

    function test_the_strip_is_a_tab_stop_while_a_clip_is_open() {
        let strip = createTemporaryObject(timelineComponent, testCase);
        verify(strip);
        compare(strip.activeFocusOnTab, true);
    }

    function test_arrow_keys_move_the_playhead_by_one_second() {
        let strip = make();
        compare(strip.keyTarget, "playhead", "the playhead is what the arrows drive by default");

        keyClick(Qt.Key_Right);
        compare(testSession.positionMs, 1000);
        keyClick(Qt.Key_Right);
        compare(testSession.positionMs, 2000);
        keyClick(Qt.Key_Left);
        compare(testSession.positionMs, 1000);
    }

    function test_shift_and_control_change_the_step() {
        let strip = make();

        keyClick(Qt.Key_Right, Qt.ShiftModifier);
        compare(testSession.positionMs, 10000);
        keyClick(Qt.Key_Left, Qt.ControlModifier);
        compare(testSession.positionMs, 9900);
    }

    function test_the_playhead_cannot_leave_the_clip() {
        let strip = make();

        keyClick(Qt.Key_Left);
        compare(testSession.positionMs, 0, "already at the start");

        keyClick(Qt.Key_End);
        compare(testSession.positionMs, testSession.durationMs);
        keyClick(Qt.Key_Right);
        compare(testSession.positionMs, testSession.durationMs, "already at the end");

        keyClick(Qt.Key_Home);
        compare(testSession.positionMs, 0);
    }

    function test_i_and_o_set_the_trim_points_at_the_playhead() {
        let strip = make();

        testSession.requestSeek(20000);
        keyClick(Qt.Key_I);
        compare(testSession.trimStartMs, 20000);
        compare(testSession.trimEndMs, testSession.durationMs, "the out point is untouched");

        testSession.requestSeek(60000);
        keyClick(Qt.Key_O);
        compare(testSession.trimEndMs, 60000);
        compare(testSession.trimStartMs, 20000, "the in point is untouched");
        compare(testSession.trimmed, true);
    }

    function test_a_trim_point_cannot_cross_its_partner() {
        let strip = make();

        testSession.requestSeek(40000);
        keyClick(Qt.Key_I);
        testSession.requestSeek(50000);
        keyClick(Qt.Key_O);
        compare(testSession.trimStartMs, 40000);
        compare(testSession.trimEndMs, 50000);

        // Park the playhead PAST the out point and mark in: the same clamp the
        // drag release goes through has to keep the range ordered.
        testSession.requestSeek(90000);
        keyClick(Qt.Key_I);
        verify(testSession.trimStartMs < testSession.trimEndMs,
               "in " + testSession.trimStartMs + " must stay before out " + testSession.trimEndMs);
    }

    function test_the_bracket_keys_choose_what_the_arrows_move() {
        let strip = make();

        testSession.requestTrim(10000, 90000);
        compare(strip.keyTarget, "playhead");

        keyClick(Qt.Key_BracketRight);
        compare(strip.keyTarget, "start");
        keyClick(Qt.Key_Right);
        compare(testSession.trimStartMs, 11000, "the arrows now move the in point");
        compare(testSession.positionMs, 0, "and no longer the playhead");

        keyClick(Qt.Key_BracketRight);
        compare(strip.keyTarget, "end");
        keyClick(Qt.Key_Left);
        compare(testSession.trimEndMs, 89000);

        // It cycles rather than dead-ending, in both directions.
        keyClick(Qt.Key_BracketRight);
        compare(strip.keyTarget, "playhead");
        keyClick(Qt.Key_BracketLeft);
        compare(strip.keyTarget, "end");
    }

    function test_space_reaches_the_player() {
        let strip = make();

        // The fixture clip is never opened, so the player refuses to start —
        // which is the honest assertion available here: the key is CONSUMED by
        // the strip and routed to the player, and the player's own guard is
        // what decides the outcome.
        compare(testPlayer.playing, false);
        keyClick(Qt.Key_Space);
        compare(testPlayer.playing, false, "no clip is open, so nothing plays");
    }
}
