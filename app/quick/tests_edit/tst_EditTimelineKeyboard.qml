import QtQuick
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: testCase
    name: "EditTimelineKeyboard"
    when: windowShown
    width: 640
    height: 240
    visible: true
    Component {
        id: textInputComponent
        TextInput { width: 200; height: 30; text: "Editable name" }
    }
    Component {
        id: timelineComponent
        EditTimeline {
            session: testSession
            player: testPlayer
            width: 600
            height: 200
        }
    }
    function init() {
        while (testSession.canUndo) testSession.undo();
        testSession.appendAsset(testSession.media[0].id);
        testSession.requestSeek(0);
    }
    function make() {
        const strip = createTemporaryObject(timelineComponent, testCase);
        verify(strip);
        strip.forceActiveFocus();
        verify(strip.activeFocus);
        return strip;
    }
    function test_focus_and_seek() {
        const strip = make();
        compare(strip.activeFocusOnTab, true);
        verify(strip.Accessible.name.length > 0);
        keyClick(Qt.Key_Right);
        compare(testSession.positionMs, 33);
        keyClick(Qt.Key_Right, Qt.ShiftModifier);
        compare(testSession.positionMs, 1033);
        keyClick(Qt.Key_End);
        compare(testSession.positionMs, testSession.durationMs);
        keyClick(Qt.Key_Home);
        compare(testSession.positionMs, 0);
    }
    function test_text_input_keeps_edit_shortcuts() {
        make();
        testSession.requestSeek(50000);
        const input = createTemporaryObject(textInputComponent, testCase);
        input.forceActiveFocus();
        verify(input.activeFocus);
        keyClick(Qt.Key_B, Qt.ControlModifier);
        keyClick(Qt.Key_Delete);
        compare(testSession.visibleClips(0, 100000).length, 2);
    }
    function test_split_and_undo_redo() {
        make();
        testSession.requestSeek(50000);
        keyClick(Qt.Key_B, Qt.ControlModifier);
        compare(testSession.visibleClips(0, 100000).length, 4);
        keyClick(Qt.Key_Z, Qt.ControlModifier);
        compare(testSession.visibleClips(0, 100000).length, 2);
        keyClick(Qt.Key_Y, Qt.ControlModifier);
        compare(testSession.visibleClips(0, 100000).length, 4);
    }
    function test_keyboard_trim_move_and_delete() {
        make();
        keyClick(Qt.Key_Right, Qt.AltModifier | Qt.ControlModifier);
        compare(testSession.visibleClips(0, 100000)[0].inMs, 100);
        keyClick(Qt.Key_Right, Qt.AltModifier);
        compare(testSession.visibleClips(0, 100000)[0].startMs, 200);
        keyClick(Qt.Key_Delete);
        compare(testSession.visibleClips(0, 100000).length, 0);
        keyClick(Qt.Key_Z, Qt.ControlModifier);
        compare(testSession.visibleClips(0, 100000).length, 2);
    }
    function test_keyboard_ripple_closes_gap() {
        make();
        testSession.requestSeek(50000);
        keyClick(Qt.Key_B, Qt.ControlModifier);
        keyClick(Qt.Key_Delete, Qt.ShiftModifier);
        compare(testSession.durationMs, 50000);
        compare(testSession.visibleClips(0, 100000)[0].startMs, 0);
    }
    function test_viewport_bounds_objects_for_long_timeline() {
        make();
        testSession.appendAsset(testSession.media[0].id);
        compare(testSession.visibleClips(0, 99999).length, 2);
        compare(testSession.visibleClips(100001, 200000).length, 2);
    }
}
