import QtQuick
import QtTest

import ExoSnap.Quick.EditTestControls

// The Edit player's transport toggle.
//
// The picture is the subject of this surface, so the toggle is only drawn when
// it has something to say: paused, focused, or just after a transport change. It
// disappears while the clip plays and while the playhead is being dragged, and
// it does so WITHOUT leaving the tab order -- the control was made a real
// AbstractButton (QCR-504) precisely so it could be reached by keyboard, and
// hiding it with `visible` would take that back.
//
// `testSession` and `testTransportPlayer` are real adapters built by the runner
// (edit_timeline_qml_test_main.cpp), seeded with a 100 s fixture clip that has
// no master path, so nothing is ever decoded.
TestCase {
    id: testCase

    name: "EditPlayerTransport"
    when: windowShown
    width: 640
    height: 400
    visible: true

    Component {
        id: playerComponent

        EditPlayer {
            session: testSession
            player: testTransportPlayer
            width: 600
            height: 340
        }
    }

    function init() {
        // The adapters are shared across the whole run, so each case has to put
        // the transport back rather than assume where the last one left it.
        testTransportPlayer.setPlaying(false);
        testTransportPlayer.endScrub();
    }

    function findToggle(player) {
        return findChild(player, "editPlayerToggle");
    }

    function test_the_toggle_stands_while_the_preview_is_paused() {
        let player = createTemporaryObject(playerComponent, testCase);
        verify(player);
        let toggle = findToggle(player);
        verify(toggle);

        verify(!testTransportPlayer.playing);
        tryCompare(toggle, "opacity", 1.0);
        verify(toggle.visible);
    }

    function test_playing_clears_the_toggle_once_the_hold_expires() {
        let player = createTemporaryObject(playerComponent, testCase);
        verify(player);
        let toggle = findToggle(player);
        verify(toggle);

        testTransportPlayer.setPlaying(true);
        compare(testTransportPlayer.playing, true);
        // The hold is deliberate: the glyph swaps to pause and has to be legible
        // before it goes. It is the fade that ends, not the control.
        tryCompare(toggle, "opacity", 0.0, 4000);
        verify(toggle.visible, "the toggle must stay in the tab order while faded out");
        verify(toggle.enabled, "a faded toggle still answers Enter and Space");
    }

    function test_a_scrub_clears_the_toggle_immediately() {
        let player = createTemporaryObject(playerComponent, testCase);
        verify(player);
        let toggle = findToggle(player);
        verify(toggle);

        // A scrub from the paused state, which is the case that used to leave a
        // circle sitting over the frame the drag was looking for.
        verify(!testTransportPlayer.playing);
        testTransportPlayer.beginScrub();
        compare(testTransportPlayer.scrubbing, true);
        tryCompare(toggle, "opacity", 0.0, 4000);

        testTransportPlayer.endScrub();
        compare(testTransportPlayer.scrubbing, false);
        tryCompare(toggle, "opacity", 1.0, 4000);
    }

    function test_a_scrub_of_a_playing_clip_does_not_flash_the_toggle_on_release() {
        let player = createTemporaryObject(playerComponent, testCase);
        verify(player);
        let toggle = findToggle(player);
        verify(toggle);

        testTransportPlayer.setPlaying(true);
        tryCompare(toggle, "opacity", 0.0, 4000);

        // The adapter pauses for the drag and resumes on release. Both are its
        // own doing, so neither may be reported as a transport change the user
        // made -- a hold triggered here would fade a pause glyph in over the
        // frame the scrub just found.
        testTransportPlayer.beginScrub();
        compare(testTransportPlayer.playing, false);
        compare(toggle.opacity, 0.0);
        testTransportPlayer.endScrub();
        compare(testTransportPlayer.playing, true);
        compare(toggle.opacity, 0.0);
    }
}
